// Spinel packed unsigned integer ("i", EXI-based): 7 bits a byte, low first, the high bit set on every byte but the
// last. The spinel spec (spinel.h, "Packed Unsigned Integer") limits the value to what three bytes hold, and OpenThread
// (spinel.c) refuses a value at or above SPINEL_MAX_UINT_PACKED (2,097,151): commands, properties, status codes and
// caps all fit below it, and a longer or larger encoding is malformed.
// Header-only and pure (no ESP-IDF calls): used by radio_mux's detection, br_core's RCP probe and the OpenThread port's
// answers after GONE (p4/ot_port, which is compiled into the generated openthread component and gets a link to it).
#pragma once

#include <stddef.h>
#include <stdint.h>

#define RADIO_MUX_SPINEL_UINT_MAX 2097150u // SPINEL_MAX_UINT_PACKED - 1: OpenThread requires a value below it
#define RADIO_MUX_SPINEL_UINT_LEN_MAX 3

// Encodes v into out (room for RADIO_MUX_SPINEL_UINT_LEN_MAX bytes); returns the bytes written, 0 when v is above
// RADIO_MUX_SPINEL_UINT_MAX (nothing written).
static inline size_t radio_mux_spinel_uint_pack(uint32_t v, uint8_t *out)
{
    size_t n = 0;

    if (v > RADIO_MUX_SPINEL_UINT_MAX) {
        return 0;
    }
    while (v >= 0x80) {
        out[n++] = (uint8_t)(v | 0x80);
        v >>= 7;
    }
    out[n++] = (uint8_t)v;
    return n;
}

// Decodes from the len bytes at p; returns the bytes consumed, 0 (and *v untouched) when the input ends inside the
// number or the number runs past RADIO_MUX_SPINEL_UINT_LEN_MAX bytes or
// is above RADIO_MUX_SPINEL_UINT_MAX.
static inline size_t radio_mux_spinel_uint_unpack(const uint8_t *p, size_t len, uint32_t *v)
{
    uint32_t r = 0;

    for (size_t i = 0; i < len && i < RADIO_MUX_SPINEL_UINT_LEN_MAX; i++) {
        r |= (uint32_t)(p[i] & 0x7F) << (7 * i);
        if (!(p[i] & 0x80)) {
            if (r > RADIO_MUX_SPINEL_UINT_MAX) {
                return 0;
            }
            *v = r;
            return i + 1;
        }
    }
    return 0;
}
