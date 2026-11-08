#pragma once

// /device/console bodies (console_json.c). Pure: host-tested in test_host/.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * The GET and PUT /device/console body: {"enabled":<bool>,"session":null} or, with a peer,
 * {"enabled":<bool>,"session":{"peer":<peer>,"since_s":<since_s>}}. Returns its length, or -1 when `cap` is too small.
 */
int device_api_console_json(char *out, size_t cap, bool enabled, const char *peer, uint32_t since_s);

/** Parse a PUT /device/console body: an object with a boolean "enabled". ESP_ERR_INVALID_ARG otherwise. */
esp_err_t device_api_console_parse(const char *body, bool *enabled);

/** Detail of the 503 the Thread REST API and the console WebSocket answer in NO_RADIO. */
#define DEVICE_API_NO_RADIO_DETAIL "no radio in OTBR mode"

/**
 * Whether the console WebSocket handshake is refused in board mode `mode` (a br_core_mode_t): 503 in NO_RADIO (detail
 * DEVICE_API_NO_RADIO_DETAIL), 0 otherwise (the console itself says when Thread is not running).
 */
int device_api_console_ws_refusal(int mode);

#ifdef __cplusplus
}
#endif
