// What OpenThread's spinel interface answers for a request after its radio channel is gone (mux_spinel_interface.cpp):
// an answer OpenThread accepts without starting its RCP recovery or tripping an assert, or none (the request times out,
// where OpenThread copes with nothing else: requests made only at its init). Pure: no ESP-IDF calls; spinel.h for the values.
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
    OT_MUX_GONE_CHANNELS, // the value of a channel mask GET: the 2.4 GHz channels 11 to 26 (its caller dies on errors)
} ot_mux_gone_t;

// The longest answer frame (a CHANNELS answer: header, command, a two-byte property, 16 channels).
#define OT_MUX_GONE_FRAME_MAX 24

// The answer for a request with spinel command `cmd` about property `key`.
ot_mux_gone_t ot_mux_gone_answer(uint32_t cmd, uint32_t key);
// The TID, command and property of an outgoing spinel frame. false when it is not a command with a property.
bool ot_mux_gone_parse(const uint8_t *frame, size_t len, uint8_t *tid, uint32_t *cmd, uint32_t *key);
// The answer frame for `tid` (`key`: the property the request was about): PROP_VALUE_IS LAST_STATUS for OK and FAILURE
// (the status) and TX_ABORT (the status, stack-native ABORT, and the two transmit-done booleans frame pending and
// header updated: false); PROP_VALUE_IS `key` with the channels 11 to 26 for CHANNELS. 0 for WAIT.
size_t ot_mux_gone_frame(ot_mux_gone_t answer, uint8_t tid, uint32_t key, uint8_t out[OT_MUX_GONE_FRAME_MAX]);
// Whether an outgoing frame is a spinel RESET command (TID 0; SpinelDriver waits for a reset notification after it).
bool ot_mux_gone_is_reset(const uint8_t *frame, size_t len);
// The reset notification that answers a reset after GONE: LAST_STATUS RESET_UNKNOWN with TID 0.
size_t ot_mux_gone_reset_frame(uint8_t out[OT_MUX_GONE_FRAME_MAX]);

// The main loop's use of the channel (MuxSpinelInterface's UpdateFdSet). After GONE the channel's eventfd stays out
// of the select: nothing comes from the channel any more, the answers after GONE need no wakeup, and an eventfd left
// readable (an event still queued, or the next consumer's after a new attach) would make the select return at once on
// every pass.
typedef struct {
    bool watch_fd; // put the channel's eventfd into the select
    bool no_wait;  // the select must not wait (a zero timeout)
} ot_mux_loop_t;

// `gone`: the channel is gone. `frames_waiting`: a frame may still wait (one was just taken, or one is held back).
// `answers_due`: a request or a reset waits for its answer (answered only after GONE; before, the RCP answers).
ot_mux_loop_t ot_mux_loop_plan(bool gone, bool frames_waiting, bool answers_due);
// Whether Process takes from the channel this pass (`readable`: the select found the eventfd readable). After GONE on
// every pass: the take drains what is still queued (slots back to the pool, the eventfd cleared) and answers what is
// due.
bool ot_mux_loop_take(bool gone, bool frames_waiting, bool readable);

#ifdef __cplusplus
}
#endif
