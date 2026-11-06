#pragma once

/*
 * Spinel frames an RCP sends, for the spinel client's host tests.
 *
 * These frames are CONSTRUCTED, not captured: the boards were busy, so nothing was recorded from OpenThread's
 * RadioSpinel talking to the C6 or the CC2652P. Each builder packs its frame with OpenThread's own spinel.c
 * (spinel_datatype_pack, ESP-IDF 6.1, compiled by path into this test) in the layout the RCP side writes and
 * RadioSpinel parses. The function each one mirrors is named next to it:
 *   - OpenThread's RCP: src/ncp/ncp_base.cpp, src/ncp/ncp_base_radio.cpp (NcpBase);
 *   - the host: src/lib/spinel/radio_spinel.cpp (RadioSpinel), src/lib/spinel/spinel_driver.cpp (SpinelDriver).
 *
 * The two RCP profiles: the C6's radio caps are 0x1FF (ESP-IDF's host port requires 0x1FF of it and the board runs on
 * it); the CC2652P's are 0x7F (no rx-timing, no rx-on-when-idle), as read from the dongle on the board on 2026-11-06.
 * The CAPS lists, versions and EUI-64s are made up in the shape those RCPs report.
 */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include "spinel.h"

/* spinel.h's SPINEL_HEADER_IID() uses static_cast; the plain-C header byte. */
#define FRAMES_HEADER(iid, tid) ((uint8_t)(SPINEL_HEADER_FLAG | ((iid) << SPINEL_HEADER_IID_SHIFT) | (tid)))

/* OT_RADIO_CAPS_* (openthread/platform/radio.h). */
#define FRAMES_CAPS_ACK_TIMEOUT (1u << 0)
#define FRAMES_CAPS_ENERGY_SCAN (1u << 1)
#define FRAMES_CAPS_TRANSMIT_RETRIES (1u << 2)
#define FRAMES_CAPS_CSMA_BACKOFF (1u << 3)
#define FRAMES_CAPS_RX_ON_WHEN_IDLE (1u << 8)

#define FRAMES_C6_RADIO_CAPS 0x1FFu
#define FRAMES_CC2652P_RADIO_CAPS 0x7Fu

/* OT_ERROR_ABORT wrapped by NcpBase::ThreadErrorToSpinelStatus (no spinel status of its own). */
#define FRAMES_STATUS_ABORT (SPINEL_STATUS_STACK_NATIVE__BEGIN + 11)

/* Header, command, property, then `fmt` (NcpBase's mEncoder.BeginFrame + its writes). */
static inline size_t frames_prop(uint8_t *buf, size_t size, uint8_t header, unsigned cmd, unsigned key,
                                 const char *fmt, ...)
{
    spinel_ssize_t n = spinel_datatype_pack(buf, size, "Cii", header, cmd, key);
    va_list args;

    va_start(args, fmt);
    spinel_ssize_t m = spinel_datatype_vpack(buf + n, size - (size_t)n, fmt, args);
    va_end(args);
    return (size_t)(n + m);
}

/* NcpBase::CommandHandler_RESET: LAST_STATUS RESET_POWER_ON as a notification (TID 0) on IID `iid`. */
static inline size_t frames_reset_status(uint8_t *buf, size_t size, uint8_t iid, unsigned status)
{
    return frames_prop(buf, size, FRAMES_HEADER(iid, 0), SPINEL_CMD_PROP_VALUE_IS, SPINEL_PROP_LAST_STATUS, "i",
                       status);
}

/* NcpBase::PrepareLastStatusResponse / WriteLastStatusFrame: the reply to a request that failed (or set a property
 * that has no getter, with SPINEL_STATUS_OK). */
static inline size_t frames_last_status(uint8_t *buf, size_t size, uint8_t tid, unsigned status)
{
    return frames_prop(buf, size, FRAMES_HEADER(0, tid), SPINEL_CMD_PROP_VALUE_IS, SPINEL_PROP_LAST_STATUS, "i",
                       status);
}

/* NcpBase::WritePropertyValueIsFrame: the reply to a get, or to a set of a property with a getter. */
static inline size_t frames_value_is(uint8_t *buf, size_t size, uint8_t tid, unsigned key, const uint8_t *value,
                                     size_t len)
{
    return frames_prop(buf, size, FRAMES_HEADER(0, tid), SPINEL_CMD_PROP_VALUE_IS, key, "D", value, (uint32_t)len);
}

/* NcpBase::WritePropertyValueInsertedRemovedFrame: the reply to an insert. */
static inline size_t frames_inserted(uint8_t *buf, size_t size, uint8_t tid, unsigned key, const uint8_t *value,
                                     size_t len)
{
    return frames_prop(buf, size, FRAMES_HEADER(0, tid), SPINEL_CMD_PROP_VALUE_INSERTED, key, "D", value,
                       (uint32_t)len);
}

/* NcpBase::HandlePropertyGet<PROTOCOL_VERSION>: "ii" major, minor. */
static inline size_t frames_protocol_version(uint8_t *buf, size_t size, uint8_t tid, unsigned major, unsigned minor)
{
    return frames_prop(buf, size, FRAMES_HEADER(0, tid), SPINEL_CMD_PROP_VALUE_IS, SPINEL_PROP_PROTOCOL_VERSION,
                       "ii", major, minor);
}

/* NcpBase::HandlePropertyGet<NCP_VERSION>: "U". */
static inline size_t frames_ncp_version(uint8_t *buf, size_t size, uint8_t tid, const char *version)
{
    return frames_prop(buf, size, FRAMES_HEADER(0, tid), SPINEL_CMD_PROP_VALUE_IS, SPINEL_PROP_NCP_VERSION, "U",
                       version);
}

/* NcpBase::HandlePropertyGet<CAPS>: packed uints, parsed by SpinelDriver::HandleInitialFrame. */
static inline size_t frames_caps(uint8_t *buf, size_t size, uint8_t tid, const unsigned *caps, size_t count)
{
    uint8_t list[32];
    size_t len = 0;

    for (size_t i = 0; i < count; i++) {
        len += (size_t)spinel_packed_uint_encode(list + len, sizeof(list) - len, caps[i]);
    }
    return frames_value_is(buf, size, tid, SPINEL_PROP_CAPS, list, len);
}

/* NcpBase::HandlePropertyGet<RADIO_CAPS> (ncp_base_radio.cpp): "i". */
static inline size_t frames_radio_caps(uint8_t *buf, size_t size, uint8_t tid, unsigned caps)
{
    return frames_prop(buf, size, FRAMES_HEADER(0, tid), SPINEL_CMD_PROP_VALUE_IS, SPINEL_PROP_RADIO_CAPS, "i",
                       caps);
}

/* NcpBase::HandlePropertyGet<HWADDR>: "E". */
static inline size_t frames_hwaddr(uint8_t *buf, size_t size, uint8_t tid, const uint8_t eui64[8])
{
    return frames_prop(buf, size, FRAMES_HEADER(0, tid), SPINEL_CMD_PROP_VALUE_IS, SPINEL_PROP_HWADDR, "E", eui64);
}

/*
 * NcpBase::LinkRawReceiveDone + PackRadioFrame: STREAM_RAW notification "d c c S t(C C X) t(i) t(C L)" (frame with
 * its FCS, RSSI, noise floor -128, flags, PHY data, vendor data with the receive error, MAC data), parsed by
 * RadioSpinel::ParseRadioFrame. `psdu_len` includes the FCS. With rx_error != 0 the RCP sends a zero-length frame.
 */
static inline size_t frames_stream_raw_rx(uint8_t *buf, size_t size, uint8_t iid, const uint8_t *psdu,
                                          size_t psdu_len, int8_t rssi, uint8_t channel, uint8_t lqi,
                                          uint64_t timestamp, unsigned rx_error)
{
    return frames_prop(buf, size, FRAMES_HEADER(iid, 0), SPINEL_CMD_PROP_VALUE_IS, SPINEL_PROP_STREAM_RAW,
                       "dccSt(CCX)t(i)t(CL)", psdu, (uint32_t)(rx_error ? 0 : psdu_len), rssi, -128, 0, channel,
                       lqi, timestamp, rx_error, 0, (uint32_t)0);
}

/*
 * NcpBase::LinkRawTransmitDone: LAST_STATUS on the transmit's TID, "i b b" (status, frame pending, header updated),
 * then the ACK as PackRadioFrame when the status is OK; parsed by RadioSpinel::HandleTransmitDone.
 */
static inline size_t frames_tx_done(uint8_t *buf, size_t size, uint8_t tid, unsigned status)
{
    static const uint8_t ack[5] = {0x02, 0x00, 0x2a, 0x00, 0x00}; // imm-ack, seq 42, FCS
    spinel_ssize_t n = (spinel_ssize_t)frames_prop(buf, size, FRAMES_HEADER(0, tid), SPINEL_CMD_PROP_VALUE_IS,
                                                   SPINEL_PROP_LAST_STATUS, "ibb", status, false, true);

    if (status == SPINEL_STATUS_OK) {
        n += spinel_datatype_pack(buf + n, size - (size_t)n, "dccSt(CCX)t(i)t(CL)", ack, (uint32_t)sizeof(ack),
                                  -60, -128, 0, 15, 200, (uint64_t)0, 0, 0, (uint32_t)0);
    }
    return (size_t)n;
}

/* NcpBase::HandlePropertySet_SPINEL_PROP_STREAM_RAW when otLinkRawTransmit refuses: LAST_STATUS with the status only. */
static inline size_t frames_tx_refused(uint8_t *buf, size_t size, uint8_t tid, unsigned status)
{
    return frames_last_status(buf, size, tid, status);
}

/* NcpBase::LinkRawEnergyScanDone: MAC_ENERGY_SCAN_RESULT "Cc" (channel, max RSSI) as a notification. */
static inline size_t frames_energy_result(uint8_t *buf, size_t size, uint8_t channel, int8_t max_rssi)
{
    return frames_prop(buf, size, FRAMES_HEADER(0, 0), SPINEL_CMD_PROP_VALUE_IS, SPINEL_PROP_MAC_ENERGY_SCAN_RESULT,
                       "Cc", channel, max_rssi);
}

/* The RCPs' attach answers. */
static const unsigned FRAMES_RCP_CAPS[] = {SPINEL_CAP_COUNTERS, SPINEL_CAP_UNSOL_UPDATE_FILTER, SPINEL_CAP_MAC_RAW,
                                           SPINEL_CAP_CONFIG_RADIO, SPINEL_CAP_RCP_API_VERSION,
                                           SPINEL_CAP_RCP_MIN_HOST_API_VERSION
                                          };
static const uint8_t FRAMES_C6_EUI64[8] = {0x40, 0x4c, 0xca, 0xff, 0xfe, 0x5b, 0x1a, 0x10};
static const uint8_t FRAMES_CC2652P_EUI64[8] = {0x00, 0x12, 0x4b, 0x00, 0x2a, 0x3b, 0x4c, 0x5d};
#define FRAMES_C6_NCP_VERSION "openthread-esp32/c6; 2026-10-20 09:00:00 UTC"
#define FRAMES_CC2652P_NCP_VERSION "OPENTHREAD/1.4.0; CC2652P; Oct 21 2026 10:00:00"
