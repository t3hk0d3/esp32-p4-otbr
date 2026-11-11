// USB/IP server (spec docs/superpowers/specs/2026-11-10-usbip-design.md): exports the one USB device over TCP port
// USBIP_PORT. A stored flag decides at boot whether USB/IP or the radio mux owns the USB port; changing it takes a
// reboot.
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "usbip_core.h"
#include "usbip_status.h"

#ifdef __cplusplus
extern "C" {
#endif

// Eventfds the module creates: add to the eventfd VFS max_fds before usbip_boot.
#define USBIP_EVENTFDS 1
// lwIP sockets the module holds at most: the listener, the session, the pending connections (net.c PENDING_MAX, 2)
// and one just accepted while the oldest pending one is being evicted. While USB/IP is active the RCP proxy has no
// USB channel (radio_mux treats the USB port as external), so the two never hold sockets at once.
#define USBIP_SOCKETS 5

// Reads the stored flag (NVS namespace "usbip", key "enabled"; missing means off) and decides who owns the USB port.
// Enabled and no USB channel stored in otbr or ziggurat: marks the radio mux's USB transport external
// (radio_mux_set_usb_external) and starts the network side, which starts the USB host. Otherwise nothing starts. Call
// once, before radio_mux_start and after NVS and the eventfd VFS are ready. An error leaves the radio mux in charge.
esp_err_t usbip_boot(void);

// Whether USB/IP owns the USB port this boot. Any task.
bool usbip_active(void);

// Stores the flag when the verdict is USBIP_SET_REBOOT (the caller reboots; this never does). long_op: a long
// operation runs (it blocks enabling). USBIP_SET_REFUSE_IN_USE fills detail with
// "<channel id> runs <mode>: move it or switch it off first". detail may be NULL. An NVS failure on the store is
// reported as USBIP_SET_STORE_FAILED after logging (nothing stored).
usbip_set_verdict_t usbip_set_enabled(bool on, bool long_op, char *detail, size_t cap);

// Ends the running session, if any. ESP_ERR_INVALID_STATE when USB/IP is not active.
esp_err_t usbip_kick(void);

// Fills every field (spec section 9). Any task except the OpenThread and esp_timer tasks.
void usbip_get_status(usbip_status_t *out);

#ifdef __cplusplus
}
#endif
