// USB serial proxy (spec docs/superpowers/specs/2026-11-05-usb-serial-proxy-design.md): a USB serial dongle on the
// USB-A port served byte for byte to one TCP client. One owner task owns every state change; this API posts to it.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "usb_serial_core.h"

#ifdef __cplusplus
extern "C" {
#endif

// lwIP sockets the proxy holds at most: the listener, the client and one connection being refused.
#define USB_SERIAL_SOCKETS 3

typedef struct {
    bool present; // a USB device is attached
    uint16_t vid, pid;
    usb_serial_driver_t driver; // USB_SERIAL_DRV_NONE: unsupported, left closed
    bool open;                  // opened and bridged
    bool flow_on;               // hardware flow control is actually on
    char error[64];             // "" or a short reason
} usb_serial_dongle_t;

typedef struct {
    bool present;
    char peer[48];
    uint32_t since_s;
} usb_serial_client_t;

typedef struct {
    usb_serial_dongle_t dongle;
    usb_serial_client_t client;
    uint64_t bytes_to_dongle, bytes_from_dongle, discarded_from_client;
    uint32_t overflow_disconnects, refused_connections;
} usb_serial_status_t;

// Loads the stored settings (defaults when nothing is stored) and starts the owner task, which starts the USB host
// and the listener when the stored setting says enabled.
esp_err_t usb_serial_init(void);

// The stored settings (the defaults before init).
void usb_serial_get_settings(usb_serial_settings_t *out);

// Checks, stores and applies `s`, and returns once the owner task has applied it. ESP_ERR_INVALID_ARG on a failed
// check (nothing stored), ESP_ERR_INVALID_STATE before init, ESP_ERR_TIMEOUT when the owner did not confirm within
// 5 s, an NVS error when storing failed.
esp_err_t usb_serial_apply(const usb_serial_settings_t *s);

// Closes the connected client, if any; the next connection is taken. ESP_ERR_INVALID_STATE before init.
esp_err_t usb_serial_kick(void);

// A snapshot of the owner task's status.
void usb_serial_get_status(usb_serial_status_t *out);

#ifdef __cplusplus
}
#endif
