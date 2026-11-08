#include "radio_mux_hdlc.h"

#define ESC 0x7D
#define ESC_XOR 0x20
// The running register after a frame and its own (complemented) CRC: the CRC-16/X.25 residue, PPP's "good FCS".
#define CRC_GOOD 0xF0B8
#define CRC_INIT 0xFFFF

static uint16_t crc_update(uint16_t crc, uint8_t b)
{
    crc ^= b;
    for (int i = 0; i < 8; i++) {
        crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0x8408) : (uint16_t)(crc >> 1);
    }
    return crc;
}

static bool must_escape(uint8_t b)
{
    return b == RADIO_MUX_HDLC_FLAG || b == ESC || b == 0x11 || b == 0x13 || b == 0xF8;
}

// Bytes b takes on the wire.
static size_t wire_len(uint8_t b)
{
    return must_escape(b) ? 2 : 1;
}

static size_t put(uint8_t *out, size_t at, uint8_t b)
{
    if (must_escape(b)) {
        out[at++] = ESC;
        b ^= ESC_XOR;
    }
    out[at++] = b;
    return at;
}

size_t radio_mux_hdlc_encode(const uint8_t *frame, size_t n, uint8_t *out, size_t cap)
{
    uint16_t crc = CRC_INIT;
    size_t need = 2, at = 0;

    for (size_t i = 0; i < n; i++) {
        crc = crc_update(crc, frame[i]);
        need += wire_len(frame[i]);
    }
    crc = (uint16_t)~crc;
    need += wire_len(crc & 0xFF) + wire_len(crc >> 8);
    if (need > cap) {
        return 0;
    }

    out[at++] = RADIO_MUX_HDLC_FLAG;
    for (size_t i = 0; i < n; i++) {
        at = put(out, at, frame[i]);
    }
    at = put(out, at, crc & 0xFF);
    at = put(out, at, crc >> 8);
    out[at++] = RADIO_MUX_HDLC_FLAG;
    return at;
}

static void restart_frame(radio_mux_hdlc_dec_t *d)
{
    d->len = 0;
    d->esc = false;
    d->crc = CRC_INIT;
}

void radio_mux_hdlc_dec_set_buf(radio_mux_hdlc_dec_t *d, uint8_t *buf, size_t cap)
{
    d->buf = buf;
    d->cap = cap;
    restart_frame(d);
}

void radio_mux_hdlc_dec_init(radio_mux_hdlc_dec_t *d, uint8_t *buf, size_t cap)
{
    d->in_frame = false;
    d->crc_errors = 0;
    d->overruns = 0;
    radio_mux_hdlc_dec_set_buf(d, buf, cap);
}

size_t radio_mux_hdlc_decode_feed(radio_mux_hdlc_dec_t *d, const uint8_t *in, size_t n, bool *frame_done)
{
    size_t i;

    *frame_done = false;
    for (i = 0; i < n;) {
        uint8_t b = in[i++];

        if (b == RADIO_MUX_HDLC_FLAG) {
            bool aborted = d->esc;
            size_t len = d->len;
            bool good = d->in_frame && !aborted && len > RADIO_MUX_HDLC_CRC_LEN && d->crc == CRC_GOOD;

            if (d->in_frame && len > 0 && !good) {
                d->crc_errors++;
            } else if (d->in_frame && aborted) {
                d->crc_errors++;
            }
            d->in_frame = true;
            if (good) {
                d->len = len - RADIO_MUX_HDLC_CRC_LEN;
                *frame_done = true;
                return i;
            }
            restart_frame(d);
            continue;
        }
        if (!d->in_frame) {
            continue;
        }
        if (b == ESC) {
            d->esc = true;
            continue;
        }
        if (d->esc) {
            b ^= ESC_XOR;
            d->esc = false;
        }
        if (d->len >= d->cap) {
            d->overruns++;
            d->in_frame = false;
            restart_frame(d);
            continue;
        }
        d->buf[d->len++] = b;
        d->crc = crc_update(d->crc, b);
    }
    return i;
}

size_t radio_mux_hdlc_chunk(const uint8_t *in, size_t n, size_t *frame_len)
{
    size_t start = n; // n = no opening flag yet

    *frame_len = 0;
    for (size_t i = 0; i < n; i++) {
        if (in[i] != RADIO_MUX_HDLC_FLAG) {
            continue;
        }
        if (start < n && i > start + 1) {
            *frame_len = i - start + 1;
            return i;
        }
        start = i;
    }
    return 0;
}
