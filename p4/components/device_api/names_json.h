#pragma once

// /device/names bodies (names_json.c). Pure: host-tested in test_host/. Apart from device_api_priv.h so that the
// components that include that header by path do not need netnames.

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "netnames.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * The GET /device/names body (spec section 6), or with `res` the PUT reply: the same plus "applied" and, when not empty,
 * "detail". Returns its length, or -1 when `cap` is too small.
 */
int device_api_names_json(char *buf, size_t cap, const netnames_t *names, bool split, const netnames_result_t *res);

/** 400 body of an invalid name: {"error":"invalid_config","field":<field>,"detail":"<field>: <reason>"}. -1 when too small. */
int device_api_names_error_json(char *buf, size_t cap, const char *field, const char *reason);

/**
 * Parse a PUT /device/names body: an object with all five fields, strings of at most 63 bytes. ESP_ERR_INVALID_ARG with
 * *field = the bad key ("body" when it is not a JSON object); the values themselves are checked by netnames_set.
 */
esp_err_t device_api_names_parse(const char *body, netnames_settings_t *out, const char **field);

#ifdef __cplusplus
}
#endif
