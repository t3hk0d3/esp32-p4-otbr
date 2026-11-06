#include "otbr_probe.h"

#include <stdio.h>
#include <string.h>

#define TID_MAX 15

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

size_t otbr_probe_reset_frame(uint8_t out[OTBR_PROBE_FRAME_MAX])
{
    out[0] = OTBR_SPINEL_HEADER_FLAG;
    out[1] = OTBR_SPINEL_CMD_RESET;
    out[2] = OTBR_SPINEL_RESET_STACK;
    return 3;
}

size_t otbr_probe_get_frame(uint8_t tid, uint32_t prop, uint8_t out[OTBR_PROBE_FRAME_MAX])
{
    if (tid == 0 || tid > TID_MAX) {
        return 0;
    }
    out[0] = OTBR_SPINEL_HEADER_FLAG | tid;
    out[1] = OTBR_SPINEL_CMD_PROP_VALUE_GET;
    return 2 + pack_uint(prop, &out[2]);
}

bool otbr_probe_parse(const uint8_t *frame, size_t len, otbr_spinel_msg_t *out)
{
    size_t n, at = 1;

    if (!frame || len < 3 || (frame[0] & 0xC0) != OTBR_SPINEL_HEADER_FLAG) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->tid = frame[0] & 0x0F;
    n = unpack_uint(&frame[at], len - at, &out->cmd);
    if (n == 0) {
        return false;
    }
    at += n;
    n = unpack_uint(&frame[at], len - at, &out->key);
    if (n == 0) {
        return false;
    }
    at += n;
    out->value = &frame[at];
    out->value_len = len - at;
    return true;
}

bool otbr_probe_is_reset(const otbr_spinel_msg_t *m)
{
    uint32_t status;

    return m->tid == 0 && m->cmd == OTBR_SPINEL_CMD_PROP_VALUE_IS && m->key == OTBR_SPINEL_PROP_LAST_STATUS &&
           unpack_uint(m->value, m->value_len, &status) > 0 && status >= OTBR_SPINEL_STATUS_RESET_BEGIN &&
           status < OTBR_SPINEL_STATUS_RESET_END;
}

bool otbr_probe_is_answer(const otbr_spinel_msg_t *m, uint8_t tid, uint32_t key)
{
    return m->tid == tid && m->cmd == OTBR_SPINEL_CMD_PROP_VALUE_IS && m->key == key;
}

bool otbr_probe_has_cap(const uint8_t *value, size_t len, uint32_t cap)
{
    size_t at = 0;

    while (at < len) {
        uint32_t c;
        const size_t n = unpack_uint(&value[at], len - at, &c);

        if (n == 0) {
            return false;
        }
        if (c == cap) {
            return true;
        }
        at += n;
    }
    return false;
}

bool otbr_probe_radio_caps(const uint8_t *value, size_t len, uint32_t *caps)
{
    return unpack_uint(value, len, caps) > 0;
}

uint32_t otbr_probe_missing_caps(uint32_t caps, char *out, size_t cap)
{
    static const struct {
        uint32_t bit;
        const char *name;
    } names[] = {
        {OTBR_RADIO_CAP_ACK_TIMEOUT, "ack-timeout"},
        {OTBR_RADIO_CAP_TRANSMIT_RETRIES, "tx-retries"},
        {OTBR_RADIO_CAP_CSMA_BACKOFF, "csma-backoff"},
        {OTBR_RADIO_CAP_TRANSMIT_SEC, "tx-security"},
        {OTBR_RADIO_CAP_TRANSMIT_TIMING, "tx-timing"},
    };
    const uint32_t missing = OTBR_RADIO_CAPS_REQUIRED & ~caps;
    size_t used = 0;

    if (cap == 0) {
        return missing;
    }
    out[0] = '\0';
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]) && missing; i++) {
        if (missing & names[i].bit) {
            const int n = snprintf(out + used, cap - used, "%s%s", used ? ", " : "missing radio caps: ", names[i].name);

            if (n < 0 || (size_t)n >= cap - used) {
                break;
            }
            used += (size_t)n;
        }
    }
    return missing;
}
