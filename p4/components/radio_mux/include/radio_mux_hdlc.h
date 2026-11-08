// HDLC-lite framing of spinel frames (as OpenThread's Hdlc): 0x7E flags, 0x7D escapes, CRC-16/X.25. Pure: no ESP-IDF
// calls.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RADIO_MUX_HDLC_FLAG 0x7E
#define RADIO_MUX_HDLC_CRC_LEN 2
// Largest encoding of an n-byte frame: two flags and every frame or CRC byte escaped.
#define RADIO_MUX_HDLC_ENCODED_MAX(n) (2 + 2 * ((n) + RADIO_MUX_HDLC_CRC_LEN))

// Encodes one frame: 0x7E, the frame and its CRC-16/X.25 (little-endian) with 0x7E, 0x7D, 0x11, 0x13 and 0xF8
// escaped as 0x7D, byte ^ 0x20, then 0x7E. Returns the bytes written, or 0 (nothing written) when `cap` is short.
size_t radio_mux_hdlc_encode(const uint8_t *frame, size_t n, uint8_t *out, size_t cap);

// Streaming decoder. `buf` receives the unescaped frame followed by its two CRC bytes, so a slot of cap bytes holds
// frames of up to cap - 2 bytes. Any escaped byte is accepted. `crc` is the running register.
typedef struct {
    uint8_t *buf;
    size_t cap, len;
    bool esc, in_frame;
    uint16_t crc;
    uint32_t crc_errors, overruns;
} radio_mux_hdlc_dec_t;

void radio_mux_hdlc_dec_init(radio_mux_hdlc_dec_t *d, uint8_t *buf, size_t cap);

// Hands the decoder a fresh buffer (after a completed frame) and clears the frame in progress. Counters and the
// synchronisation state are kept.
void radio_mux_hdlc_dec_set_buf(radio_mux_hdlc_dec_t *d, uint8_t *buf, size_t cap);

// Consumes input until a frame completes or the input ends; returns the bytes consumed. *frame_done tells whether
// a good frame completed: d->len is then its length without the CRC and the bytes are in d->buf. The caller takes the
// frame and calls radio_mux_hdlc_dec_set_buf (or _init) before feeding again; the rest of `in` is fed in the next
// call. Dropped frames (bad CRC, shorter than the CRC, aborted by 0x7D 0x7E) count in crc_errors; frames that
// don't fit count in overruns and the decoder waits for the next 0x7E. Empty frames (repeated flags) emit nothing.
size_t radio_mux_hdlc_decode_feed(radio_mux_hdlc_dec_t *d, const uint8_t *in, size_t n, bool *frame_done);

// UDP framing: finds the first complete frame (an opening 0x7E, at least one byte, a closing 0x7E) in a byte stream.
// Returns the bytes to drop from the front of `in` once the frame is taken, 0 when no complete frame is there yet.
// *frame_len is the frame's length with both flags (0 when none); the frame starts at in + (return - *frame_len + 1).
// The closing flag is not dropped: it opens the next frame when the sender shares flags. Bytes before an opening
// flag and repeated flags are dropped with the frame; with no complete frame nothing is dropped, so the caller
// bounds its buffer.
size_t radio_mux_hdlc_chunk(const uint8_t *in, size_t n, size_t *frame_len);

#ifdef __cplusplus
}
#endif
