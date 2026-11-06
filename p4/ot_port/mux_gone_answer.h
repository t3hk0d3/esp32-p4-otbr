// What OpenThread's spinel interface answers for a request after its radio channel is gone (mux_spinel_interface.cpp):
// an answer OpenThread accepts without starting its RCP recovery or tripping an assert, or none (the request times out,
// where OpenThread copes with nothing else). Pure: no ESP-IDF calls; spinel.h for the values.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    OT_MUX_GONE_WAIT,     // no answer: the request times out
    OT_MUX_GONE_OK,       // LAST_STATUS OK: a SET, INSERT or REMOVE "worked"
    OT_MUX_GONE_FAILURE,  // LAST_STATUS FAILURE: a GET whose caller takes an error
    OT_MUX_GONE_TX_ABORT, // the transmit-done answer with status ABORT (SubMac takes kErrorAbort)
} ot_mux_gone_t;

// The longest answer frame.
#define OT_MUX_GONE_FRAME_MAX 8

// The answer for a request with spinel command `cmd` about property `key`.
ot_mux_gone_t ot_mux_gone_answer(uint32_t cmd, uint32_t key);
// The TID, command and property of an outgoing spinel frame. false when it is not a command with a property.
bool ot_mux_gone_parse(const uint8_t *frame, size_t len, uint8_t *tid, uint32_t *cmd, uint32_t *key);
// The answer frame (PROP_VALUE_IS LAST_STATUS) for `tid`: OK and FAILURE carry the status, TX_ABORT the status
// (stack-native ABORT) and the two transmit-done booleans (frame pending, header updated: false). 0 for WAIT.
size_t ot_mux_gone_frame(ot_mux_gone_t answer, uint8_t tid, uint8_t out[OT_MUX_GONE_FRAME_MAX]);

#ifdef __cplusplus
}
#endif
