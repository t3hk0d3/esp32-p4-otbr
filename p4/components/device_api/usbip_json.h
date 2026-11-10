#pragma once

// GET and PUT /device/usbip bodies (usbip_json.c). Pure: host-tested in test_host/.

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "usbip_status.h"

#ifdef __cplusplus
extern "C" {
#endif

/** A buffer that holds the body for any status: the longest strings, every escape, every number at its widest. */
#define USBIP_JSON_MAX 1536

/** The largest PUT body the handler reads. */
#define USBIP_JSON_PUT_MAX 128

/**
 * The /device/usbip body (spec section 9): {enabled, active, pending_reboot, state, reason, port, interface, device,
 * client, counters, memory, last_error}. `reason` and `last_error` are null when empty; `device` and `client` are
 * null when absent. device.vid/pid/bcd_device are 4 lowercase hex digits, class 2; speed is low|full|high|unknown.
 * ESP_ERR_INVALID_SIZE when it does not fit `cap`; `buf` then holds a truncated, NUL-terminated prefix.
 * `*len` is the length written.
 */
esp_err_t usbip_json_status(const usbip_status_t *st, char *buf, size_t cap, size_t *len);

typedef enum {
    USBIP_PUT_SET,  // {"enabled": bool}: *enabled holds the value
    USBIP_PUT_KICK, // {"kick": true}
} usbip_put_kind_t;

/**
 * Parses a PUT body: exactly {"enabled": bool} or {"kick": true}. ESP_ERR_INVALID_ARG for anything else (not an
 * object, extra or both keys, wrong types, kick false, duplicate keys).
 */
esp_err_t usbip_json_put_parse(const char *body, usbip_put_kind_t *kind, bool *enabled);

#ifdef __cplusplus
}
#endif
