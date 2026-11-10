// USB/IP in-flight and waiting memory budget (spec section 8.1). Pure bookkeeping, no I/O and no allocation: the
// session engine asks whether a transfer may go to the USB hardware now or must wait in its endpoint's FIFO.
//
// Endpoint index: 0 = EP0, 1..USBIP_EP_MAX = the claimed non-control endpoints (the engine maps addresses to indices).
//
// Grant rule, in order:
// - small reserve: an endpoint with nothing in flight always gets a transfer of up to USBIP_RESERVE_SMALL (2 KB), even
//   when the shared bytes or the transfer count are used up (HCI commands, EP0, always-pending HID/HCI reads);
// - shared: an IN endpoint (not EP0) with transfers in flight gets one more while in-flight bytes + n <=
//   USBIP_INFLIGHT_BYTES (256 KB) and in-flight transfers < USBIP_INFLIGHT_XFERS (32);
// - large reserve: an endpoint with nothing in flight gets a larger first transfer (OUT, EP0, or an IN whose shared
//   check fails) while the charged bytes after the grant stay <= USBIP_INFLIGHT_HARD (408 KB);
// - otherwise the transfer waits in its FIFO. OUT endpoints and EP0 have at most one transfer in flight.
// Every grant counts toward the in-flight bytes and transfers, so the shared check sees reserve grants too.
//
// Charged bytes: the sum over all USBIP_EP_MAX + 1 endpoint indices of max(the endpoint's in-flight bytes,
// USBIP_RESERVE_SMALL). An idle endpoint is charged the 2 KB its small reserve may take at any moment, so a large
// grant leaves room for every small reserve still to come. Small grants and releases never raise the charge.
//
// PSRAM worst case (transfer buffers are DMA PSRAM): in-flight bytes <= charged bytes <= USBIP_INFLIGHT_HARD (408 KB):
// 256 KB IN shared, one 120 KB large transfer and the 16 x 2 KB small reserves. Shared and small grants alone stay at
// or below 288 KB, so one 120 KB OUT or EP0 transfer always fits once no other large transfer is in flight.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "usbip_core.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    size_t inflight;
    size_t inflight_peak;
    size_t waiting;
    size_t waiting_peak;
} usbip_mem_stats_t;

typedef struct {
    uint16_t ep_inflight[USBIP_EP_MAX + 1];  // transfers in flight per endpoint index
    size_t ep_bytes[USBIP_EP_MAX + 1];       // bytes in flight per endpoint index
    size_t inflight_bytes;
    size_t charged;                          // sum of max(ep_bytes, USBIP_RESERVE_SMALL), see above
    size_t inflight_xfers;
    usbip_mem_stats_t stats;
} usbip_budget_t;

void usbip_budget_init(usbip_budget_t *b);

// True when the transfer may be submitted now (the bytes and the slot are then taken). An out-of-range index is refused.
bool usbip_budget_grant(usbip_budget_t *b, uint8_t ep_index, bool is_in, size_t n);

// Frees one granted transfer of n bytes. Releasing more than is in flight clamps at zero; an out-of-range index is
// ignored.
void usbip_budget_release(usbip_budget_t *b, uint8_t ep_index, size_t n);

// Accounts n bytes of a waiting URB (PSRAM). False, and nothing changes, when it would exceed USBIP_WAIT_BYTES.
bool usbip_budget_wait_add(usbip_budget_t *b, size_t n);
void usbip_budget_wait_sub(usbip_budget_t *b, size_t n);

void usbip_budget_stats(const usbip_budget_t *b, usbip_mem_stats_t *out);

#ifdef __cplusplus
}
#endif
