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
#include "usbip_status.h"

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
// A DMA transfer buffer is refused (the engine answers -ENOMEM) when it would leave less than this much internal DMA
// memory free: the rest of the firmware keeps a floor however the budget and the heap's other users add up.
#define USBIP_DMA_FREE_FLOOR (48 * 1024)
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
//   When esp-usb already reported the device gone it does nothing (esp-usb recovers and powers the port itself).
const usbip_engine_ops_t *usbip_usb_ops(void);

// Opens the device of the last USBIP_EV_DEV_NEW, builds its record (usbip_core_dev_record) and checks that every
// interface of the active configuration can be claimed at alternate setting 0 (claimed and released again: the engine
// claims them when a session starts). cfg points at esp-usb's copy of the active configuration descriptor, valid until
// usbip_usb_close. On an error the device is not held and why says what failed (the unsupported reason);
// ESP_ERR_INVALID_STATE while a device is open: a second USBIP_EV_DEV_NEW for the same device (usbip_usb_start can
// report one that the client's own NEW_DEV also reports) is then a no-op for net.c. ESP_ERR_NOT_FOUND when none was
// reported.
esp_err_t usbip_usb_open(usbip_dev_rec_t *rec, const uint8_t **cfg, size_t *cfg_len, char *why, size_t cap);

// Releases what is still claimed and closes the device. Stop the engine first: cfg becomes invalid. ESP_OK when no
// device is held afterwards (also without one). A close esp-usb refuses keeps the device (logged) and returns its
// error; calling it again retries.
esp_err_t usbip_usb_close(void);

// Whether the root port is off after a session's power-cycle and not on again yet (the USB task powers it on once the
// device is closed and freed). Network task.
bool usbip_usb_port_off(void);

// Whether no device is open and an enumerated one waits to be opened (its USBIP_EV_DEV_NEW may have found the old one
// still open). Network task.
bool usbip_usb_new_pending(void);

// The open device's manufacturer, product and serial strings (ASCII; other characters as '?'), each into a buffer of
// cap bytes; empty strings without a device or a descriptor.
void usbip_usb_strings(char *manufacturer, char *product, char *serial, size_t cap);

/* ---- network side (net.c) ---- */

// Eventfds net.c creates (its wake-up); the eventfd VFS must have room for them before usbip_net_start.
#define USBIP_EVENTFD_COUNT 1

// Creates the events queue (USBIP_USB_EVENTS_LEN), sets the USB side's wake-up, starts the USB side (usbip_usb_start)
// and the network task: the TCP USBIP_PORT listener (any address; netcfg's NETCFG_SVC_USBIP rules per connection), the
// connections and the one session. The network task is the only caller of the engine and of the USB side's other
// functions. ESP_ERR_INVALID_STATE when called twice.
esp_err_t usbip_net_start(void);

// Ends the session if one runs (asynchronously: the network task closes it). Any task; not the OpenThread or esp_timer
// task (it writes an eventfd).
void usbip_net_kick(void);

// Fills the device, client, state, reason, counters, sessions, refused, memory and last_error fields of out under the
// module's lock; leaves the other fields alone. Before usbip_net_start: state no_device, the rest left alone.
void usbip_net_snapshot(usbip_status_t *out);

#ifdef __cplusplus
}
#endif
