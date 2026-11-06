// Shared between the owner task (usb_serial.c), the USB glue (usb_host_glue.c) and the TCP side (tcp.c).
// Unless a function says otherwise, only the owner task calls it.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "usb_serial.h"

typedef enum {
    USB_SERIAL_MSG_APPLY,    // new settings (HTTP handler)
    USB_SERIAL_MSG_KICK,     // close the client (HTTP handler)
    USB_SERIAL_MSG_NEW_DEV,  // a USB device appeared (new_dev_cb)
    USB_SERIAL_MSG_GONE,     // the open dongle was unplugged (CDC event callback)
    USB_SERIAL_MSG_OVERFLOW, // the dongle -> client ring would have overflowed (CDC data callback)
} usb_serial_msg_type_t;

typedef struct {
    uint16_t vid, pid;
    uint8_t addr;
    usb_serial_driver_t driver;
} usb_serial_dev_t;

typedef struct {
    usb_serial_msg_type_t type;
    SemaphoreHandle_t done; // APPLY, KICK: given by the owner once handled (may be NULL)
    union {
        usb_serial_settings_t settings; // APPLY
        usb_serial_dev_t dev;           // NEW_DEV
        void *hdl;                      // GONE: the CDC handle that went away
    };
} usb_serial_msg_t;

// Any task or callback (never blocks): false when the queue is full or the owner is not running.
bool usb_serial_post(const usb_serial_msg_t *m);

/* ---- usb_host_glue.c ---- */

// Installs the USB host (from the owner task, so its interrupt lands on MGMT_CORE), the usb_lib task and the CDC
// driver with its new-device callback.
esp_err_t usb_serial_host_start(void);
// The USB half of the shutdown order, after the client is closed: close the dongle, cdc_acm_host_uninstall,
// usb_host_device_free_all, wait for all-free, usb_host_uninstall (in usb_lib), usb_lib ends.
void usb_serial_host_stop(void);
bool usb_serial_host_running(void);

// Opens `dev` and applies line coding, flow control and DTR+RTS (both asserted, one request). Fills `st`'s open,
// flow_on and error. ESP_OK when the dongle ended up open.
esp_err_t usb_serial_dongle_open(const usb_serial_dev_t *dev, const usb_serial_settings_t *s, usb_serial_dongle_t *st);
// Applies new baud and flow settings to the open dongle (DTR/RTS untouched). A baud rate read back far off closes it.
void usb_serial_dongle_configure(const usb_serial_settings_t *s, usb_serial_dongle_t *st);
void usb_serial_dongle_close(void);
bool usb_serial_dongle_is_open(void);
bool usb_serial_dongle_is(const void *hdl);
esp_err_t usb_serial_dongle_write(const uint8_t *data, size_t n);
// Whether a device with this address is still enumerated (an unsupported device sends no unplug event).
bool usb_serial_dev_attached(uint8_t addr);

// The dongle -> client ring. Reset empties it; `accept` says whether the data callback may fill it (a client is
// connected and the dongle is open), else it drops what arrives.
void usb_serial_rx_reset(bool accept);
// Pending bytes and when the oldest of them arrived (esp_timer microseconds).
size_t usb_serial_rx_pending(int64_t *first_us);
// The contiguous run at the head of the ring; usb_serial_rx_consume drops n bytes of it after they were sent.
size_t usb_serial_rx_peek(const uint8_t **p);
void usb_serial_rx_consume(size_t n);

/* ---- tcp.c ---- */

// A dual-stack listener on in6addr_any:port, backlog 1, non-blocking. -1 on failure.
int usb_serial_tcp_listen(uint16_t port);
// Closes with SO_LINGER {1, 0}: an RST, no TIME_WAIT.
void usb_serial_tcp_refuse(int fd);
// Client socket options (TCP_NODELAY, keepalive 10/2/3, non-blocking) and its peer address as text.
void usb_serial_tcp_client_setup(int fd, char *peer, size_t cap);
// Sends what the ring holds without blocking; what the socket does not take stays at the head of the ring. Adds the
// bytes sent to *sent. False when the connection failed (close it).
bool usb_serial_tcp_flush(int fd, uint64_t *sent);
