// Private interfaces between the usbip component's target-only parts: the esp-usb side (usb_side.c), the network
// side (net.c) and the settings and boot module.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "usbip_core.h"
#include "usbip_engine.h"
#include "usbip_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- esp-usb side (usb_side.c) ---- */

// DMA transfers (esp-usb usb_transfer_t) alive at once: one session's in-flight worst case (the shared transfers plus
// one reserve per endpoint) twice, for the orphans of an ended session whose canceled completions are still on the
// way. alloc(n, true) returns NULL beyond it (the engine answers -ENOMEM).
#define USBIP_USB_XFERS_MAX (2 * (USBIP_INFLIGHT_XFERS + USBIP_EP_MAX + 1))
// The length the events queue passed to usbip_usb_start must have: one completion per live transfer plus device
// events. The USB task blocks on a full queue (a completion is never dropped), so a shorter queue can stall it.
#define USBIP_USB_EVENTS_LEN (USBIP_USB_XFERS_MAX + 8)

// One message from the USB task to the network task.
// - USBIP_EV_DEV_NEW: a device was enumerated; usbip_usb_open opens it.
// - USBIP_EV_DEV_GONE: the device went away (unplugged, or the root port turned off); end the session, then
//   usbip_usb_close. Also sent for a device that was never opened (unsupported, or its open failed).
// - USBIP_EV_XFER_DONE: a transfer handed to ops->submit came back; hand x, st, actual to usbip_engine_on_complete.
typedef struct {
    enum { USBIP_EV_DEV_NEW, USBIP_EV_DEV_GONE, USBIP_EV_XFER_DONE } type;
    usbip_xfer_t *x;
    usbip_xfer_status_t st;
    size_t actual;
} usbip_event_t;

// Called by the USB task after each message it puts on the events queue, so the network task can wake from its
// select (an eventfd write). Set it before usbip_usb_start; NULL (the default) posts without a wake-up.
void usbip_usb_set_wake(void (*wake)(void));

// Installs the USB host from a task pinned to NET_CORE (never uninstalled), registers the usbip client, starts the
// USB task that handles its events, and posts usbip_event_t messages to events (USBIP_USB_EVENTS_LEN long). A device
// already enumerated is reported as USBIP_EV_DEV_NEW. ESP_ERR_INVALID_STATE when called twice.
esp_err_t usbip_usb_start(QueueHandle_t events);

// The engine's USB operations on esp-usb, every member except emit, end_session and now_ms (NULL here: net.c copies
// the struct and fills them). ctx is unused. All of them run on the network task only.
// - alloc(n, true) is an esp-usb transfer's data buffer (internal, DMA capable, cache aligned); the transfer is the
//   xfer's native handle and lives as long as the buffer: free(buf) frees both. alloc(n, false) is PSRAM.
// - iface_release waits (bounded) until the interface's flushed transfers have come back to the USB task.
// - port_power_cycle turns the root port off now; the USB task turns it on again once the device is closed and freed.
const usbip_engine_ops_t *usbip_usb_ops(void);

// Opens the device of the last USBIP_EV_DEV_NEW, builds its record (usbip_core_dev_record) and checks that every
// interface of the active configuration can be claimed at alternate setting 0 (claimed and released again: the engine
// claims them when a session starts). cfg points at esp-usb's copy of the active configuration descriptor, valid until
// usbip_usb_close. On an error the device is not held and why says what failed (the unsupported reason);
// ESP_ERR_INVALID_STATE while a device is open, ESP_ERR_NOT_FOUND when none was reported.
esp_err_t usbip_usb_open(usbip_dev_rec_t *rec, const uint8_t **cfg, size_t *cfg_len, char *why, size_t cap);

// Releases what is still claimed and closes the device. Stop the engine first: cfg becomes invalid. No-op without a
// device. A close esp-usb refuses keeps the device (logged); calling it again retries.
void usbip_usb_close(void);

// The open device's manufacturer, product and serial strings (ASCII; other characters as '?'), each into a buffer of
// cap bytes; empty strings without a device or a descriptor.
void usbip_usb_strings(char *manufacturer, char *product, char *serial, size_t cap);

#ifdef __cplusplus
}
#endif
