#pragma once

// /device/usb_serial bodies (usb_serial_json.c). Pure: host-tested in test_host/.

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "usb_serial.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * The GET and PUT /device/usb_serial body, fields in this order: enabled, port, baud, flow_control, dongle (null with
 * no device; vid and pid as 4 lowercase hex digits, driver null for an unsupported one), client (null with none),
 * bytes_to_dongle, bytes_from_dongle, discarded_from_client, overflow_disconnects, refused_connections.
 * Returns its length, or -1 when `cap` is too small.
 */
int device_api_usb_serial_json(char *out, size_t cap, const usb_serial_settings_t *s, const usb_serial_status_t *st);

/**
 * Parse a PUT /device/usb_serial body onto `inout`: an object with any of enabled, flow_control (booleans), port,
 * baud (numbers) and kick (a boolean, not a setting: *kick says whether it was true). Checks types only: a number that
 * is not an integer or does not fit its field becomes 0, which usb_serial_settings_check refuses.
 * ESP_ERR_INVALID_ARG for invalid JSON, a non-object or a wrong type (`inout` unchanged then).
 */
esp_err_t device_api_usb_serial_parse(const char *body, usb_serial_settings_t *inout, bool *kick);

#ifdef __cplusplus
}
#endif
