// USB/IP in-flight and waiting memory budget (spec section 8.1). Pure bookkeeping, no I/O and no allocation: the
// session engine asks whether a transfer may go to the USB hardware now or must wait in its endpoint's FIFO.
//
// Endpoint index: 0 = EP0, 1..USBIP_EP_MAX = the claimed non-control endpoints (the engine maps addresses to indices).
//
// Grant rule: an endpoint with nothing in flight is always granted (its reserve), even when the shared bytes or the
// transfer count are used up. Otherwise only an IN endpoint is granted, while in-flight bytes + n <= USBIP_INFLIGHT_BYTES
// and in-flight transfers < USBIP_INFLIGHT_XFERS. OUT endpoints and EP0 have at most one transfer in flight. Reserve
// grants count toward the in-flight bytes and transfers, so the shared check sees them.
//
// Internal RAM worst case: USBIP_INFLIGHT_BYTES (32 KB) plus one transfer per endpoint (at most USBIP_XFER_MAX, 16 KB,
// each).
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
    size_t inflight_bytes;
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
