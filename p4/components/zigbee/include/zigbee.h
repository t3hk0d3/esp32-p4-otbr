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
#include "zigbee_status.h"

#ifdef __cplusplus
extern "C" {
#endif

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
