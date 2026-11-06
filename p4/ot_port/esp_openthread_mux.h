// OpenThread's radio through the radio channel mux (spec section 3, OTBR mode). The project's MuxSpinelInterface
// (mux_spinel_interface.cpp) is compiled into a generated copy of ESP-IDF's openthread component
// (tools/ot_radio_port.cmake) and reaches the RCP only through these operations, which br_core implements over a mux
// channel. Seen by br_core through its openthread requirement.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    // Sends one spinel frame (copied before it returns). ESP_ERR_INVALID_STATE once the channel is gone.
    esp_err_t (*tx)(void *ctx, const uint8_t *f, size_t n);
    // Takes the next event without waiting: true with *slot set is a received spinel frame (give it back with
    // release), true with *slot NULL is a radio reset (RESET), false is nothing waiting (the eventfd is then cleared).
    bool (*poll)(void *ctx, uint8_t **slot, size_t *len);
    void (*release)(void *ctx, uint8_t *slot);
    // Readable while events wait (put into the main loop's select).
    int (*eventfd)(void *ctx);
    // Asks the channel for a reset (C6: the reset GPIO; dongle: close and reopen); poll reports the RESET. Does not wait.
    esp_err_t (*reset)(void *ctx);
    // True once the channel is gone (unplugged, its mode changed, a C6 job took it): from then on nothing arrives.
    bool (*gone)(void *ctx);
} esp_openthread_mux_ops_t;

// The channel the next esp_openthread_init runs on (br_core calls it before every esp_openthread_init). `ops` must
// outlive OpenThread; `ctx` is passed to every operation. Clears the init-done mark (esp_openthread_mux_init_done).
void esp_openthread_mux_set(const esp_openthread_mux_ops_t *ops, void *ctx);
// OpenThread's init on the channel finished (br_core calls it once esp_openthread_init returned ESP_OK). From then on a
// reset asked for after GONE is answered as done; before, it fails (mux_gone_answer.h, ot_mux_gone_answers_reset).
void esp_openthread_mux_init_done(void);

#ifdef __cplusplus
}
#endif
