#pragma once

// /device/time bodies (time_json.c, spec docs/superpowers/specs/2026-11-10-time-sync-design.md section 5.2). Pure:
// host-tested in test_host/.

#include <stddef.h>
#include "esp_err.h"
#include "timesync.h"

#ifdef __cplusplus
extern "C" {
#endif

/** A buffer that holds the longest GET body: every string at its longest, every number at its widest. */
#define DEVICE_API_TIME_JSON_MAX 512

/** The GET /device/time body (also the PUT reply). Returns its length, or -1 when `cap` is too small. */
int device_api_time_json(char *buf, size_t cap, const timesync_status_t *st);

/**
 * Parse a PUT /device/time body: an object with "enabled" (bool), "mode" ("auto" | "custom") and "server" (string of
 * at most 63 bytes), all required. ESP_ERR_INVALID_ARG with *field = the bad key ("body" when it is not a JSON object);
 * the server itself is checked by timesync_set_config.
 */
esp_err_t device_api_time_parse(const char *body, timesync_config_t *out, const char **field);

#ifdef __cplusplus
}
#endif
