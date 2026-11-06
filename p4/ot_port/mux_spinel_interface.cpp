// OpenThread's spinel interface over a radio mux channel (spec section 3, OTBR mode). Frames are copied from the
// channel's pool slots into OpenThread's RX frame buffer on the OpenThread task, under the OpenThread lock (Process and
// WaitForFrame), and the slots given back at once: OpenThread's frame buffer is not thread-safe, so nothing on the mux
// side touches it.
//
// A vanished radio (GONE) must not reach RadioSpinel's recovery: on a radio that is not there it ends in DieNow (an
// abort), in whichever task made the request (the HTTP server's too). So after GONE every request OpenThread sends, and
// every one still unanswered, gets a LAST_STATUS FAILURE answer from here: the call fails like a refusal of the RCP and
// returns, and br_core tears OpenThread down from its own task.
#include "mux_spinel_interface.hpp"

#include <string.h>
#include <sys/select.h>

#include "esp_log.h"
#include "esp_openthread_common_macro.h"
#include "esp_timer.h"
#include "lib/spinel/spinel.h"

namespace {

const esp_openthread_mux_ops_t *s_ops;
void *s_ctx;

constexpr uint64_t kGoneSliceUs = 50 * 1000;        // waits look at gone() at least this often (WaitForFrame: 100 ms)
constexpr int64_t kResetWaitUs = 2000 * 1000;       // HardwareReset: the RESET of the channel
constexpr int64_t kAnnounceWaitUs = 300 * 1000;     // then the RCP's own reset notification, else a software reset
constexpr uint16_t kTidMask = 0x0F;
constexpr uint64_t kUsPerS = 1000 * 1000;

int64_t NowUs(void)
{
    return esp_timer_get_time();
}

// LAST_STATUS RESET_* with TID 0: the RCP (re)started.
bool IsResetNotification(const uint8_t *aFrame, size_t aLength)
{
    return aLength >= 4 && (aFrame[0] & ~SPINEL_HEADER_IID_MASK) == SPINEL_HEADER_FLAG &&
           SPINEL_HEADER_GET_TID(aFrame[0]) == 0 && aFrame[1] == SPINEL_CMD_PROP_VALUE_IS &&
           aFrame[2] == SPINEL_PROP_LAST_STATUS && aFrame[3] >= SPINEL_STATUS_RESET__BEGIN &&
           aFrame[3] < SPINEL_STATUS_RESET__END;
}

} // namespace

extern "C" void esp_openthread_mux_set(const esp_openthread_mux_ops_t *ops, void *ctx)
{
    s_ops = ops;
    s_ctx = ctx;
}

namespace esp {
namespace openthread {

MuxSpinelInterface::MuxSpinelInterface(void)
    : mOps(nullptr)
    , mCtx(nullptr)
    , mFd(-1)
    , mMaybeMore(false)
    , mResetSeen(false)
    , mResetAnnounced(false)
    , mGoneLogged(false)
    , mPendingTids(0)
    , mReceiveFrameCallback(nullptr)
    , mReceiveFrameContext(nullptr)
    , mReceiveFrameBuffer(nullptr)
    , mRcpFailureHandler(nullptr)
{
    memset(&mInterfaceMetrics, 0, sizeof(mInterfaceMetrics));
    mInterfaceMetrics.mRcpInterfaceType = kSpinelInterfaceTypeVendor;
}

MuxSpinelInterface::~MuxSpinelInterface(void)
{
    Deinit();
}

esp_err_t MuxSpinelInterface::Enable(const esp_openthread_spi_host_config_t &aUnusedSpiConfig)
{
    (void)aUnusedSpiConfig;
    if (!s_ops) {
        ESP_LOGE(OT_PLAT_LOG_TAG, "no mux channel set (esp_openthread_mux_set)");
        return ESP_ERR_INVALID_STATE;
    }
    mOps = s_ops;
    mCtx = s_ctx;
    mFd = mOps->eventfd(mCtx);
    if (mFd < 0) {
        mOps = nullptr;
        ESP_LOGE(OT_PLAT_LOG_TAG, "the mux channel has no eventfd");
        return ESP_ERR_INVALID_STATE;
    }
    mMaybeMore = true; // frames may wait from before the start
    mPendingTids = 0;
    mGoneLogged = false;
    ESP_LOGI(OT_PLAT_LOG_TAG, "spinel over the radio mux");
    return ESP_OK;
}

esp_err_t MuxSpinelInterface::Disable(void)
{
    mOps = nullptr;
    mCtx = nullptr;
    mFd = -1;
    return ESP_OK;
}

otError MuxSpinelInterface::Init(ReceiveFrameCallback aCallback, void *aCallbackContext, RxFrameBuffer &aFrameBuffer)
{
    mReceiveFrameCallback = aCallback;
    mReceiveFrameContext = aCallbackContext;
    mReceiveFrameBuffer = &aFrameBuffer;
    return OT_ERROR_NONE;
}

void MuxSpinelInterface::Deinit(void)
{
    mReceiveFrameCallback = nullptr;
    mReceiveFrameContext = nullptr;
    mReceiveFrameBuffer = nullptr;
}

bool MuxSpinelInterface::Gone(void) const
{
    return !mOps || mOps->gone(mCtx);
}

void MuxSpinelInterface::NoteSent(const uint8_t *aFrame, uint16_t aLength)
{
    const uint8_t tid = SPINEL_HEADER_GET_TID(aFrame[0]);

    if (aLength >= 2 && tid != 0) {
        mPendingTids |= static_cast<uint16_t>(1u << tid);
    }
}

otError MuxSpinelInterface::SendFrame(const uint8_t *aFrame, uint16_t aLength)
{
    if (!aFrame || aLength == 0) {
        return OT_ERROR_INVALID_ARGS;
    }
    if (aLength > kMaxFrameSize) {
        return OT_ERROR_NO_BUFS;
    }
    NoteSent(aFrame, aLength);
    if (Gone()) {
        return OT_ERROR_NONE; // dropped; a request in it is answered by AnswerGone
    }
    const esp_err_t err = mOps->tx(mCtx, aFrame, aLength);

    if (err == ESP_ERR_INVALID_STATE) {
        return OT_ERROR_NONE; // the channel went away during the call: as above
    }
    if (err != ESP_OK) {
        ESP_LOGW(OT_PLAT_LOG_TAG, "spinel frame not sent: %s", esp_err_to_name(err));
        return OT_ERROR_FAILED;
    }
    mInterfaceMetrics.mTxFrameCount++;
    mInterfaceMetrics.mTxFrameByteCount += aLength;
    return OT_ERROR_NONE;
}

bool MuxSpinelInterface::Deliver(const uint8_t *aFrame, size_t aLength)
{
    if (!mReceiveFrameBuffer || !mReceiveFrameCallback || aLength == 0) {
        return false;
    }
    (void)mReceiveFrameBuffer->SetSkipLength(0);
    if (mReceiveFrameBuffer->GetFrameMaxLength() < aLength) {
        ESP_LOGW(OT_PLAT_LOG_TAG, "no room for a spinel frame of %u bytes: dropped", static_cast<unsigned>(aLength));
        mReceiveFrameBuffer->DiscardFrame();
        return false;
    }
    memcpy(mReceiveFrameBuffer->GetFrame(), aFrame, aLength);
    (void)mReceiveFrameBuffer->SetLength(static_cast<uint16_t>(aLength));
    const uint8_t tid = SPINEL_HEADER_GET_TID(aFrame[0]);

    if (tid != 0) {
        mPendingTids &= static_cast<uint16_t>(~(1u << tid));
    }
    if (IsResetNotification(aFrame, aLength)) {
        mResetAnnounced = true;
    }
    mInterfaceMetrics.mTransferredFrameCount++;
    mInterfaceMetrics.mTransferredValidFrameCount++;
    mInterfaceMetrics.mRxFrameCount++;
    mInterfaceMetrics.mRxFrameByteCount += aLength;
    mReceiveFrameCallback(mReceiveFrameContext);
    return true;
}

// After GONE: LAST_STATUS FAILURE for the lowest unanswered TID, as the RCP would refuse it. The two booleans after the
// status make it a complete transmit-done answer too (frame pending, header updated: both false).
bool MuxSpinelInterface::AnswerGone(void)
{
    for (uint8_t tid = 1; tid <= kTidMask; tid++) {
        if (mPendingTids & (1u << tid)) {
            const uint8_t answer[] = {
                static_cast<uint8_t>(SPINEL_HEADER_FLAG | tid), SPINEL_CMD_PROP_VALUE_IS, SPINEL_PROP_LAST_STATUS,
                SPINEL_STATUS_FAILURE, 0, 0,
            };

            mMaybeMore = true;
            return Deliver(answer, sizeof(answer));
        }
    }
    return false;
}

// One frame to OpenThread: the next one of the channel (RESETs are noted on the way), or after GONE an answer for an
// unanswered request. False when there is none.
bool MuxSpinelInterface::TakeOne(void)
{
    uint8_t *slot;
    size_t len;

    while (mOps && mOps->poll(mCtx, &slot, &len)) {
        if (!slot) {
            mResetSeen = true;
            continue;
        }
        const bool delivered = Deliver(slot, len);

        mOps->release(mCtx, slot);
        if (delivered) {
            mMaybeMore = true;
            return true;
        }
    }
    mMaybeMore = false;
    if (Gone()) {
        if (!mGoneLogged) {
            mGoneLogged = true;
            ESP_LOGW(OT_PLAT_LOG_TAG, "the radio channel is gone: requests are refused until OpenThread stops");
        }
        return AnswerGone();
    }
    return false;
}

bool MuxSpinelInterface::WaitReadable(uint64_t aTimeoutUs)
{
    fd_set readFds;
    struct timeval timeout;

    FD_ZERO(&readFds);
    FD_SET(mFd, &readFds);
    timeout.tv_sec = static_cast<time_t>(aTimeoutUs / kUsPerS);
    timeout.tv_usec = static_cast<suseconds_t>(aTimeoutUs % kUsPerS);
    return select(mFd + 1, &readFds, nullptr, nullptr, &timeout) > 0;
}

otError MuxSpinelInterface::WaitForFrame(uint64_t aTimeoutUs)
{
    const int64_t deadline = NowUs() + static_cast<int64_t>(aTimeoutUs);

    if (!mOps) {
        return OT_ERROR_RESPONSE_TIMEOUT;
    }
    for (;;) {
        if (TakeOne()) {
            return OT_ERROR_NONE;
        }
        const int64_t now = NowUs();

        if (Gone() || now >= deadline) {
            return OT_ERROR_RESPONSE_TIMEOUT;
        }
        const uint64_t left = static_cast<uint64_t>(deadline - now);

        WaitReadable(left < kGoneSliceUs ? left : kGoneSliceUs);
    }
}

void MuxSpinelInterface::UpdateFdSet(void *aMainloopContext)
{
    esp_openthread_mainloop_context_t *ctx = static_cast<esp_openthread_mainloop_context_t *>(aMainloopContext);

    if (mFd < 0) {
        return;
    }
    if (mMaybeMore || (mPendingTids != 0 && Gone())) {
        ctx->timeout.tv_sec = 0;
        ctx->timeout.tv_usec = 0;
    }
    FD_SET(mFd, &ctx->read_fds);
    FD_SET(mFd, &ctx->error_fds);
    if (mFd > ctx->max_fd) {
        ctx->max_fd = mFd;
    }
}

void MuxSpinelInterface::Process(const void *aMainloopContext)
{
    const esp_openthread_mainloop_context_t *ctx = static_cast<const esp_openthread_mainloop_context_t *>(aMainloopContext);

    if (mFd < 0) {
        return;
    }
    if (mMaybeMore || FD_ISSET(mFd, &ctx->read_fds) || (mPendingTids != 0 && Gone())) {
        TakeOne();
    }
}

// After the channel's RESET: a C6 announces its reset by itself; a dongle reopened over USB does not, so it gets a
// spinel software reset. Frames that arrive meanwhile go to OpenThread as usual.
void MuxSpinelInterface::AfterReset(void)
{
    const int64_t deadline = NowUs() + kAnnounceWaitUs;

    while (!mResetAnnounced && !Gone() && NowUs() < deadline) {
        if (!TakeOne()) {
            WaitReadable(kGoneSliceUs);
        }
    }
    if (!mResetAnnounced && !Gone()) {
        const uint8_t reset[] = {SPINEL_HEADER_FLAG, SPINEL_CMD_RESET, SPINEL_RESET_STACK};

        ESP_LOGI(OT_PLAT_LOG_TAG, "no reset notification from the RCP: spinel software reset");
        (void)SendFrame(reset, sizeof(reset));
    }
}

otError MuxSpinelInterface::HardwareReset(void)
{
    uint8_t *slot;
    size_t len;

    if (Gone()) {
        return OT_ERROR_NONE; // nothing can reset a radio that is not there: the reset fails, br_core is told
    }
    mResetSeen = false;
    mResetAnnounced = false;
    if (mRcpFailureHandler) {
        mRcpFailureHandler(); // br_core: its recovery ladder resets the channel (or holds it, after too many)
    } else {
        (void)mOps->reset(mCtx);
    }
    // Frames from before the reset are stale: dropped.
    const int64_t deadline = NowUs() + kResetWaitUs;

    while (!mResetSeen && !Gone() && NowUs() < deadline) {
        if (mOps->poll(mCtx, &slot, &len)) {
            if (slot) {
                mOps->release(mCtx, slot);
            } else {
                mResetSeen = true;
            }
        } else {
            WaitReadable(kGoneSliceUs);
        }
    }
    mPendingTids = 0; // the RCP forgot every request
    if (mResetSeen) {
        mMaybeMore = true;
        AfterReset();
    } else if (!Gone()) {
        ESP_LOGW(OT_PLAT_LOG_TAG, "the radio channel did not report a reset within 2 s");
    }
    return OT_ERROR_NONE;
}

} // namespace openthread
} // namespace esp
