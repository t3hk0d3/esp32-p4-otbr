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

#ifdef __cplusplus
}
#endif
