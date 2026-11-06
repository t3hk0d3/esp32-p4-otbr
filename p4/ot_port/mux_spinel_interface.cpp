// OpenThread's spinel interface over a radio mux channel (spec section 3, OTBR mode). Frames are copied from the
// channel's pool slots into OpenThread's RX frame buffer on the OpenThread task, under the OpenThread lock (Process and
// WaitForFrame), and the slots given back at once: OpenThread's frame buffer is not thread-safe, so nothing on the mux
// side touches it.
//
// A vanished radio (GONE) must not reach RadioSpinel's recovery: on a radio that is not there it ends in DieNow (an
// abort), in whichever task made the request (the HTTP server's too). So after GONE every request OpenThread sends, and
// every one still unanswered, gets the answer mux_gone_answer.h picks for its command: SETs, INSERTs and REMOVEs
// "work", a frame to send ends with ABORT, a GET whose caller takes an error fails, a channel mask GET gets the 2.4 GHz
// channels; the call returns, and br_core tears OpenThread down from its own task (its teardown is only SETs and the
// transmit abort). The answers need no wakeup: after GONE the channel's eventfd stays out of the main loop's select and
// what is still queued is drained, not delivered (ot_mux_loop_plan, br_core's mux_poll). A reset asked for after GONE
// (RadioSpinel's recovery of a request that timed out before GONE) is answered by a reset notification, so the recovery
// completes instead of calling the reset-failure callback, as long as the RCP answered once since Enable. GETs read
// only at OpenThread's init get no answer: during the init, on the OpenThread task, their timeout and a failed reset
// end in the reset-failure callback (br_core parks that task).
#include "mux_spinel_interface.hpp"

#include <cinttypes>
#include <string.h>
#include <sys/select.h>

#include "esp_log.h"
#include "esp_openthread_common_macro.h"
#include "esp_timer.h"
#include "lib/spinel/spinel.h"
#include "mux_gone_answer.h"

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
    , mGoneLogged(false)
    , mRcpSeen(false)
    , mResetWaiting(false)
    , mPendingTids(0)
    , mTxDropped(0)
    , mHeldLength(0)
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
    mHeldLength = 0;
    mGoneLogged = false;
    mRcpSeen = false;
    mResetWaiting = false;
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
    uint8_t tid;
    uint32_t cmd, key;

    if (ot_mux_gone_parse(aFrame, aLength, &tid, &cmd, &key) && tid != 0) {
        mPendingTids |= static_cast<uint16_t>(1u << tid);
        mPendingCmd[tid] = cmd;
        mPendingKey[tid] = key;
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
    if (ot_mux_gone_is_reset(aFrame, aLength)) {
        mResetWaiting = true; // SpinelDriver waits for the reset notification next
    }
    if (Gone()) {
        return OT_ERROR_NONE; // dropped; a request in it is answered by AnswerGone
    }
    const esp_err_t err = mOps->tx(mCtx, aFrame, aLength);

    if (err != ESP_OK) {
        // Dropped (GONE during the call, a stalled device, a busy RCP): an error here would assert in SubMac; the
        // request's own timeout drives the usual recovery, and after GONE AnswerGone answers it.
        if (err != ESP_ERR_INVALID_STATE && (mTxDropped++ % 32) == 0) {
            ESP_LOGW(OT_PLAT_LOG_TAG, "spinel frame not sent: %s (%" PRIu32 " dropped)", esp_err_to_name(err),
                     mTxDropped);
        }
        return OT_ERROR_NONE;
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
    mInterfaceMetrics.mTransferredFrameCount++;
    mInterfaceMetrics.mTransferredValidFrameCount++;
    mInterfaceMetrics.mRxFrameCount++;
    mInterfaceMetrics.mRxFrameByteCount += aLength;
    mReceiveFrameCallback(mReceiveFrameContext);
    return true;
}

// A frame from the RCP: it answered since Enable; its reset notification answers a reset asked for.
void MuxSpinelInterface::NoteRcp(const uint8_t *aFrame, size_t aLength)
{
    mRcpSeen = true;
    if (IsResetNotification(aFrame, aLength)) {
        mResetWaiting = false;
    }
}

// After GONE: a reset asked for gets its notification (the RCP forgot every request with it), but only once the RCP
// answered since Enable: during OpenThread's init a reset that cannot be done must fail (the init's requests after it
// get no answer, and the reset-failure callback parks the OpenThread task instead of a SuccessOrDie abort).
bool MuxSpinelInterface::AnswerReset(void)
{
    uint8_t answer[OT_MUX_GONE_FRAME_MAX];

    if (!mResetWaiting || !mRcpSeen) {
        return false;
    }
    mResetWaiting = false;
    mPendingTids = 0;
    mMaybeMore = true;
    return Deliver(answer, ot_mux_gone_reset_frame(answer));
}

// After GONE: the answer for the lowest unanswered TID (mux_gone_answer.h). A request that gets none is forgotten: its
// own timeout follows.
bool MuxSpinelInterface::AnswerGone(void)
{
    for (uint8_t tid = 1; tid <= kTidMask; tid++) {
        if (!(mPendingTids & (1u << tid))) {
            continue;
        }
        uint8_t answer[OT_MUX_GONE_FRAME_MAX];
        const size_t len = ot_mux_gone_frame(ot_mux_gone_answer(mPendingCmd[tid], mPendingKey[tid]), tid,
                                             mPendingKey[tid], answer);

        if (len == 0) {
            mPendingTids &= static_cast<uint16_t>(~(1u << tid));
            continue;
        }
        mMaybeMore = true;
        return Deliver(answer, len);
    }
    return false;
}

void MuxSpinelInterface::Hold(const uint8_t *aFrame, size_t aLength)
{
    if (aLength <= sizeof(mHeld)) {
        memcpy(mHeld, aFrame, aLength);
        mHeldLength = static_cast<uint16_t>(aLength);
    }
}

// One frame to OpenThread: the next one of the channel (RESETs are noted on the way), or after GONE an answer for an
// unanswered request. False when there is none.
bool MuxSpinelInterface::TakeOne(void)
{
    uint8_t *slot;
    size_t len;

    if (mHeldLength > 0) { // the frame HardwareReset put back
        const size_t held = mHeldLength;

        NoteRcp(mHeld, held);
        mHeldLength = 0;
        mMaybeMore = true;
        if (Deliver(mHeld, held)) {
            return true;
        }
    }
    while (mOps && mOps->poll(mCtx, &slot, &len)) {
        if (!slot) {
            mResetSeen = true;
            continue;
        }
        NoteRcp(slot, len);
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
        return AnswerReset() || AnswerGone();
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
    const ot_mux_loop_t plan = ot_mux_loop_plan(Gone(), mMaybeMore || mHeldLength > 0,
                                                mPendingTids != 0 || mResetWaiting);

    if (plan.no_wait) {
        ctx->timeout.tv_sec = 0;
        ctx->timeout.tv_usec = 0;
    }
    if (!plan.watch_fd) {
        return; // gone: the select waits for OpenThread's own timeout (at most 10 s) and the other fds
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
    if (ot_mux_loop_take(Gone(), mMaybeMore || mHeldLength > 0, FD_ISSET(mFd, &ctx->read_fds))) {
        TakeOne();
    }
}

// After the channel's RESET: a C6 announces its reset by itself; a dongle reopened over USB does not, so it gets a
// spinel software reset. The first frame is only looked at: it is put back for OpenThread (SpinelDriver's
// ResetCoprocessor waits for that reset notification once HardwareReset returns).
void MuxSpinelInterface::AfterReset(void)
{
    const int64_t deadline = NowUs() + kAnnounceWaitUs;
    uint8_t *slot;
    size_t len;
    bool announced = false;

    while (mHeldLength == 0 && !Gone() && NowUs() < deadline) {
        if (!mOps->poll(mCtx, &slot, &len)) {
            WaitReadable(kGoneSliceUs);
        } else if (slot) {
            NoteRcp(slot, len);
            announced = IsResetNotification(slot, len);
            Hold(slot, len);
            mOps->release(mCtx, slot);
        }
    }
    if (!announced && !Gone()) {
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
        // Nothing can reset a radio that is not there. Past the init the reset is answered as done (AnswerReset);
        // during it the reset fails and br_core's reset-failure callback parks the OpenThread task.
        mResetWaiting = true;
        return OT_ERROR_NONE;
    }
    mResetSeen = false;
    mHeldLength = 0;
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
                NoteRcp(slot, len);
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
    if (Gone() && mHeldLength == 0) {
        mResetWaiting = true; // went during the reset: answered as done once the RCP had answered (AnswerReset)
    }
    return OT_ERROR_NONE;
}

} // namespace openthread
} // namespace esp
