// OpenThread's SpinelInterface over a radio mux channel (esp_openthread_mux.h). Compiled into the generated copy of
// ESP-IDF's openthread component, next to the patched esp_openthread_radio_spinel.cpp that uses it in place of
// SpiSpinelInterface (tools/ot_radio_port.cmake, PATCHES.md "Generated ESP-IDF copies").
#pragma once

#include "esp_err.h"
#include "esp_openthread_mux.h"
#include "esp_openthread_types.h"
#include "lib/spinel/spinel_interface.hpp"

namespace esp {
namespace openthread {

class MuxSpinelInterface : public ot::Spinel::SpinelInterface {
public:
    MuxSpinelInterface(void);
    ~MuxSpinelInterface(void) override;

    otError Init(ReceiveFrameCallback aCallback, void *aCallbackContext, RxFrameBuffer &aFrameBuffer) override;
    void Deinit(void) override;
    // Passes the frame to the channel. After GONE the frame is dropped and OT_ERROR_NONE returned; a request in it is
    // then answered with LAST_STATUS FAILURE (see AnswerGone), so OpenThread sees a failed request, never a timeout
    // that starts its RCP recovery (which ends in DieNow on a vanished radio).
    otError SendFrame(const uint8_t *aFrame, uint16_t aLength) override;
    // One frame into OpenThread's RX frame buffer, or OT_ERROR_RESPONSE_TIMEOUT; within 100 ms after GONE.
    otError WaitForFrame(uint64_t aTimeoutUs) override;
    void UpdateFdSet(void *aMainloopContext) override;
    void Process(const void *aMainloopContext) override;
    // ESP-IDF's otPlatRadioGetBusSpeed reads the platform config, which br_core fills per channel.
    uint32_t GetBusSpeed(void) const override
    {
        return 0;
    }
    // The registered failure handler (br_core: its recovery ladder resets the channel), else a channel reset; then up
    // to 2 s for the RESET. A dongle's reopen does not reset its RCP, so a spinel software reset follows when the RCP
    // does not announce a reset by itself. Returns at once after GONE.
    otError HardwareReset(void) override;
    const otRcpInterfaceMetrics *GetRcpInterfaceMetrics(void) const override
    {
        return &mInterfaceMetrics;
    }

    // Called by ESP-IDF's radio spinel port (the concrete-only calls of its SPI interface).
    void RegisterRcpFailureHandler(esp_openthread_rcp_failure_handler aHandler)
    {
        mRcpFailureHandler = aHandler;
    }
    // The SPI settings of ESP-IDF's call are not used: the channel comes from esp_openthread_mux_set.
    esp_err_t Enable(const esp_openthread_spi_host_config_t &aUnusedSpiConfig);
    esp_err_t Disable(void);
    esp_err_t AfterRadioInit(void)
    {
        return ESP_OK;
    }

private:
    bool TakeOne(void);
    bool Deliver(const uint8_t *aFrame, size_t aLength);
    bool AnswerGone(void);
    bool Gone(void) const;
    void NoteSent(const uint8_t *aFrame, uint16_t aLength);
    bool WaitReadable(uint64_t aTimeoutUs);
    void AfterReset(void);

    const esp_openthread_mux_ops_t *mOps;
    void *mCtx;
    int mFd;
    bool mMaybeMore;        // a frame was taken: more may wait (the eventfd stays set until a poll finds none)
    bool mResetSeen;        // poll reported a RESET
    bool mResetAnnounced;   // a spinel reset notification arrived (LAST_STATUS RESET_*, TID 0)
    bool mGoneLogged;
    uint16_t mPendingTids;  // requests sent and not answered yet (bit = spinel TID 1..15)

    ReceiveFrameCallback mReceiveFrameCallback;
    void *mReceiveFrameContext;
    RxFrameBuffer *mReceiveFrameBuffer;
    esp_openthread_rcp_failure_handler mRcpFailureHandler;
    otRcpInterfaceMetrics mInterfaceMetrics;
};

} // namespace openthread
} // namespace esp
