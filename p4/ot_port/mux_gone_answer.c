#include "mux_gone_answer.h"

#include "spinel.h"

// The 2.4 GHz IEEE 802.15.4 channels: what a CHANNELS answer reports as supported and preferred.
#define OT_MUX_GONE_CHANNEL_MIN 11
#define OT_MUX_GONE_CHANNEL_MAX 26

// Spinel packed unsigned integer (EXI): 7 bits a byte, low first, the high bit set on all but the last.
static size_t unpack_uint(const uint8_t *p, size_t len, uint32_t *out)
{
    uint32_t value = 0;

    for (size_t i = 0; i < len && i < 4; i++) {
        value |= (uint32_t)(p[i] & 0x7F) << (7 * i);
        if (!(p[i] & 0x80)) {
            *out = value;
            return i + 1;
        }
    }
    return 0;
}

static size_t pack_uint(uint32_t value, uint8_t *out)
{
    size_t n = 0;

    do {
        out[n] = value & 0x7F;
        value >>= 7;
        if (value) {
            out[n] |= 0x80;
        }
        n++;
    } while (value);
    return n;
}

// GETs whose callers in RadioSpinel and ESP-IDF's port take an error (they log it and return it, or a default value).
// The channel masks (GetRadioChannelMask, e.g. the CLI's `channel supported` in any task) end in SuccessOrDie on an
// error: they get a value. The others (HWADDR, the versions and caps) are read only at OpenThread's init, on its own
// task, and end in SuccessOrDie too: they get no answer, and the timeout leads to the reset-failure callback, which
// br_core turns into a parked OpenThread task.
static bool get_takes_error(uint32_t key)
{
    switch (key) {
    case SPINEL_PROP_PHY_TX_POWER:
    case SPINEL_PROP_PHY_CCA_THRESHOLD:
    case SPINEL_PROP_PHY_FEM_LNA_GAIN:
    case SPINEL_PROP_PHY_RSSI:
    case SPINEL_PROP_PHY_RX_SENSITIVITY:
    case SPINEL_PROP_PHY_REGION_CODE:
    case SPINEL_PROP_RADIO_COEX_ENABLE:
    case SPINEL_PROP_RADIO_COEX_METRICS:
    case SPINEL_PROP_RCP_TIMESTAMP:
    case SPINEL_PROP_MULTIPAN_ACTIVE_INTERFACE:
    case SPINEL_PROP_RCP_CSL_ACCURACY:
    case SPINEL_PROP_RCP_CSL_UNCERTAINTY:
        return true;
    default:
        return false;
    }
}

ot_mux_gone_t ot_mux_gone_answer(uint32_t cmd, uint32_t key)
{
    switch (cmd) {
    case SPINEL_CMD_PROP_VALUE_SET:
        // A frame to send: SubMac asserts on any transmit error but ABORT, NO_ACK and CCA failure.
        return key == SPINEL_PROP_STREAM_RAW ? OT_MUX_GONE_TX_ABORT : OT_MUX_GONE_OK;
    case SPINEL_CMD_PROP_VALUE_INSERT:
    case SPINEL_CMD_PROP_VALUE_REMOVE:
        return OT_MUX_GONE_OK; // the port's setters die on any error (SuccessOrDie)
    case SPINEL_CMD_PROP_VALUE_GET:
        if (key == SPINEL_PROP_PHY_CHAN_SUPPORTED || key == SPINEL_PROP_PHY_CHAN_PREFERRED) {
            return OT_MUX_GONE_CHANNELS;
        }
        return get_takes_error(key) ? OT_MUX_GONE_FAILURE : OT_MUX_GONE_WAIT;
    default:
        return OT_MUX_GONE_WAIT; // RESET and anything else
    }
}

bool ot_mux_gone_parse(const uint8_t *frame, size_t len, uint8_t *tid, uint32_t *cmd, uint32_t *key)
{
    size_t n, at = 1;

    if (!frame || len < 3 || (frame[0] & 0xC0) != SPINEL_HEADER_FLAG) {
        return false;
    }
    *tid = SPINEL_HEADER_GET_TID(frame[0]);
    n = unpack_uint(&frame[at], len - at, cmd);
    if (n == 0) {
        return false;
    }
    at += n;
    return unpack_uint(&frame[at], len - at, key) > 0;
}

bool ot_mux_gone_is_reset(const uint8_t *frame, size_t len)
{
    return frame && len >= 2 && (frame[0] & 0xC0) == SPINEL_HEADER_FLAG && frame[1] == SPINEL_CMD_RESET;
}

size_t ot_mux_gone_reset_frame(uint8_t out[OT_MUX_GONE_FRAME_MAX])
{
    size_t n = 0;

    out[n++] = SPINEL_HEADER_FLAG; // TID 0: a notification
    out[n++] = SPINEL_CMD_PROP_VALUE_IS;
    out[n++] = SPINEL_PROP_LAST_STATUS;
    n += pack_uint(SPINEL_STATUS_RESET_UNKNOWN, &out[n]);
    return n;
}

size_t ot_mux_gone_frame(ot_mux_gone_t answer, uint8_t tid, uint32_t key, uint8_t out[OT_MUX_GONE_FRAME_MAX])
{
    uint32_t status;
    size_t n = 0;

    if (answer == OT_MUX_GONE_CHANNELS) {
        out[n++] = (uint8_t)(SPINEL_HEADER_FLAG | (tid & SPINEL_HEADER_TID_MASK));
        out[n++] = SPINEL_CMD_PROP_VALUE_IS;
        n += pack_uint(key, &out[n]);
        for (uint8_t channel = OT_MUX_GONE_CHANNEL_MIN; channel <= OT_MUX_GONE_CHANNEL_MAX; channel++) {
            out[n++] = channel;
        }
        return n;
    }
    switch (answer) {
    case OT_MUX_GONE_OK:
        status = SPINEL_STATUS_OK;
        break;
    case OT_MUX_GONE_FAILURE:
        status = SPINEL_STATUS_FAILURE;
        break;
    case OT_MUX_GONE_TX_ABORT:
        status = SPINEL_STATUS_STACK_NATIVE__BEGIN + 11; // OT_ERROR_ABORT
        break;
    default:
        return 0;
    }
    out[n++] = (uint8_t)(SPINEL_HEADER_FLAG | (tid & SPINEL_HEADER_TID_MASK));
    out[n++] = SPINEL_CMD_PROP_VALUE_IS;
    out[n++] = SPINEL_PROP_LAST_STATUS;
    n += pack_uint(status, &out[n]);
    if (answer == OT_MUX_GONE_TX_ABORT) {
        out[n++] = 0; // frame pending
        out[n++] = 0; // header updated
    }
    return n;
}

ot_mux_loop_t ot_mux_loop_plan(bool gone, bool frames_waiting, bool answers_due)
{
    return (ot_mux_loop_t) {
        .watch_fd = !gone, .no_wait = frames_waiting || (gone && answers_due),
    };
}

bool ot_mux_loop_take(bool gone, bool frames_waiting, bool readable)
{
    return gone || frames_waiting || readable;
}
