// Decisions of the USB serial proxy. Pure: no ESP-IDF calls.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define USB_SERIAL_DEFAULT_PORT 6638
#define USB_SERIAL_DEFAULT_BAUD 115200
// Smallest dongle -> client ring: above the 5760-byte TCP send buffer.
#define USB_SERIAL_RING_MIN 8192
#define USB_SERIAL_CP210X_FLOW_LEN 16

typedef struct {
    bool enabled;
    uint16_t port;
    uint32_t baud;
    bool flow_control;
} usb_serial_settings_t;

typedef enum {
    USB_SERIAL_OK,
    USB_SERIAL_BAD_PORT,
    USB_SERIAL_BAD_BAUD,
} usb_serial_check_t;

typedef enum {
    USB_SERIAL_DRV_NONE,
    USB_SERIAL_DRV_CP210X,
    USB_SERIAL_DRV_CH34X,
    USB_SERIAL_DRV_CDC_ACM,
} usb_serial_driver_t;

typedef enum {
    USB_SERIAL_TAKE,
    USB_SERIAL_REFUSE,
} usb_serial_accept_t;

typedef enum {
    USB_SERIAL_BAUD_EXACT,
    USB_SERIAL_BAUD_CLOSE,
    USB_SERIAL_BAUD_FAR,
} usb_serial_baud_t;

// Byte FIFO over caller-owned storage. Not thread-safe: the glue locks it.
typedef struct {
    uint8_t *data;
    size_t cap, head, len;
} usb_serial_ring_t;

void usb_serial_settings_default(usb_serial_settings_t *out);

// Port 1..65535 except the ports the board listens on (80, 179); baud from the accepted list.
// Never depends on the attached dongle.
usb_serial_check_t usb_serial_settings_check(const usb_serial_settings_t *s);

// VID/PID tables (CP210x, CH34x) first, then the CDC rule: an interface of class 0x0A plus either the device class
// or an interface of class 0x02. Anything else is NONE.
usb_serial_driver_t usb_serial_pick_driver(uint16_t vid, uint16_t pid, uint8_t dev_class,
                                           const uint8_t *intf_classes, size_t n_intf);

// "cp210x", "ch34x", "cdc_acm"; NULL for NONE.
const char *usb_serial_driver_name(usb_serial_driver_t d);

// First client keeps the port: a connection while one is connected is refused.
usb_serial_accept_t usb_serial_accept(bool client_connected);

void usb_serial_ring_init(usb_serial_ring_t *r, uint8_t *storage, size_t cap);
// All or nothing: false (ring unchanged) when n bytes do not fit.
bool usb_serial_ring_push(usb_serial_ring_t *r, const uint8_t *src, size_t n);
// Takes up to cap bytes in push order; returns how many.
size_t usb_serial_ring_take(usb_serial_ring_t *r, uint8_t *out, size_t cap);
void usb_serial_ring_clear(usb_serial_ring_t *r);

// EXACT when equal, CLOSE within 3 % (integer arithmetic), FAR otherwise (actual 0 included).
usb_serial_baud_t usb_serial_baud_verdict(uint32_t requested, uint32_t actual);

// CP210x SET_FLOW payload (CP210X_SERIAL_STATUS layout), four little-endian uint32: ulControlHandshake,
// ulFlowReplace, ulXonLimit, ulXoffLimit.
void usb_serial_cp210x_flow(bool on, uint8_t out[USB_SERIAL_CP210X_FLOW_LEN]);

#ifdef __cplusplus
}
#endif
