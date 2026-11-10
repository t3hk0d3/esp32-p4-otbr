#include "usbip_budget.h"

#include <string.h>

void usbip_budget_init(usbip_budget_t *b)
{
    memset(b, 0, sizeof(*b));
}

bool usbip_budget_grant(usbip_budget_t *b, uint8_t ep_index, bool is_in, size_t n)
{
    if (ep_index > USBIP_EP_MAX) {
        return false;
    }
    bool reserve = b->ep_inflight[ep_index] == 0;
    bool multi = is_in && ep_index != 0;
    bool shared = multi && n <= USBIP_INFLIGHT_BYTES && b->inflight_bytes + n <= USBIP_INFLIGHT_BYTES &&
                  b->inflight_xfers < USBIP_INFLIGHT_XFERS;
    if (!reserve && !shared) {
        return false;
    }
    b->ep_inflight[ep_index]++;
    b->inflight_xfers++;
    b->inflight_bytes += n;
    if (b->inflight_bytes > b->stats.inflight_peak) {
        b->stats.inflight_peak = b->inflight_bytes;
    }
    return true;
}

void usbip_budget_release(usbip_budget_t *b, uint8_t ep_index, size_t n)
{
    if (ep_index > USBIP_EP_MAX) {
        return;
    }
    if (b->ep_inflight[ep_index] > 0) {
        b->ep_inflight[ep_index]--;
        if (b->inflight_xfers > 0) {
            b->inflight_xfers--;
        }
    }
    b->inflight_bytes = n >= b->inflight_bytes ? 0 : b->inflight_bytes - n;
}

bool usbip_budget_wait_add(usbip_budget_t *b, size_t n)
{
    if (n > USBIP_WAIT_BYTES || b->stats.waiting + n > USBIP_WAIT_BYTES) {
        return false;
    }
    b->stats.waiting += n;
    if (b->stats.waiting > b->stats.waiting_peak) {
        b->stats.waiting_peak = b->stats.waiting;
    }
    return true;
}

void usbip_budget_wait_sub(usbip_budget_t *b, size_t n)
{
    b->stats.waiting = n >= b->stats.waiting ? 0 : b->stats.waiting - n;
}

void usbip_budget_stats(const usbip_budget_t *b, usbip_mem_stats_t *out)
{
    *out = b->stats;
    out->inflight = b->inflight_bytes;
}
