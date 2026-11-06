// Zigbee mode: ziggurat (zigpy's Zigbee coordinator stack, a prebuilt Rust staticlib) on a radio_mux channel in mode
// ziggurat, with its own task, and its binary protocol served over the WebSocket /zigbee
// (docs/superpowers/specs/2026-11-06-zigbee-mode-design.md).
//
// Threading: zigbee_register runs in app_main before radio_mux_start, zigbee_register_ws in the web server's
// extra-handlers callback. zigbee_get_status and zigbee_channel may be called from any task.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_http_server.h"
#include "zigbee_spinel.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZIGBEE_CHANNEL_ID_MAX 48 // RADIO_MUX_ID_MAX
#define ZIGBEE_TEXT_MAX 96
#define ZIGBEE_ADDRESS_MAX 48

typedef enum {
    ZIGBEE_STATE_IDLE,         // no channel attached yet: none in mode ziggurat, or its radio is not there
    ZIGBEE_STATE_STARTING,     // the RCP's attach sequence runs (first attach, a RESET, the radio coming back)
    ZIGBEE_STATE_RUNNING,      // the radio is up for the stack
    ZIGBEE_STATE_NO_RADIO,     // the channel went away (GONE); the stack keeps running without a radio
    ZIGBEE_STATE_INCOMPATIBLE, // the RCP cannot serve ziggurat (protocol version, radio caps); `reason` says why
} zigbee_state_t;

typedef struct {
    char channel[ZIGBEE_CHANNEL_ID_MAX]; // the channel id; "" when no channel is in mode ziggurat
    char firmware[ZIGBEE_TEXT_MAX];      // the mux's firmware record of the channel ("spinel <version> <chip>"); ""
    zigbee_state_t state;
    char reason[ZIGBEE_TEXT_MAX]; // why not running (incompatible, no radio, the mux's reason while idle); "" else
    bool has_client;
    char client_address[ZIGBEE_ADDRESS_MAX];
    int64_t client_since; // seconds since the epoch; 0 when the board clock was not set
    bool has_last_client;
    char last_client_address[ZIGBEE_ADDRESS_MAX];
    int64_t last_client_left; // seconds since the epoch; 0 when the board clock was not set
    bool has_radio_config;
    zigbee_radio_config_t radio; // the radio as the stack last configured it
    struct {
        uint32_t rx;           // frames received from the radio and handed to the stack
        uint32_t rx_dropped;   // frames the mux dropped on the channel (pool or event queue full)
        uint32_t tx;           // transmits the RCP accepted
        uint32_t tx_no_ack;    // transmit results: no ACK
        uint32_t tx_cca;       // channel access failure
        uint32_t tx_failed;    // failed or aborted, and transmits refused before reaching the RCP
        uint32_t energy_scans; // energy scans started
        uint32_t rcp_resets;   // RCP resets (the mux's RESET and the RCP's own)
        uint32_t frames_in;    // protocol frames from the client to the stack
        uint32_t frames_out;   // protocol frames sent to the client
    } counters;
    struct {
        size_t current, peak; // bytes of the stack's PSRAM heap in use
    } heap;
    bool stack_started; // ziggurat_start ran (once per boot)
    int64_t started;    // when it ran, seconds since the epoch; 0 when not started or the clock was not set
} zigbee_status_t;

/**
 * Registers the ziggurat consumer with the radio mux (firmware spinel, one channel, reboot on change, frames). Call
 * before radio_mux_start. The zigbee task and its sender are created when the mux first attaches a channel.
 */
esp_err_t zigbee_register(void);

/** Registers the WebSocket /zigbee on the web server. Call from the extra-handlers callback (before the catch-all). */
esp_err_t zigbee_register_ws(httpd_handle_t server);

/** A copy of the status, taken under a spinlock (plus the mux's view of the channel). Any task. */
void zigbee_get_status(zigbee_status_t *out);

/** The id of the channel in mode ziggurat (attached or not), or NULL when there is none. Any task. */
const char *zigbee_channel(void);

#ifdef __cplusplus
}
#endif
