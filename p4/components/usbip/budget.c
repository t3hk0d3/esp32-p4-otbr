#include "usbip_budget.h"

#include <string.h>

static size_t charge(size_t ep_bytes)
{
    return ep_bytes > USBIP_RESERVE_SMALL ? ep_bytes : USBIP_RESERVE_SMALL;
}

void usbip_budget_init(usbip_budget_t *b)
{
    memset(b, 0, sizeof(*b));
    b->charged = (size_t)(USBIP_EP_MAX + 1) * USBIP_RESERVE_SMALL;
}

bool usbip_budget_grant(usbip_budget_t *b, uint8_t ep_index, bool is_in, size_t n)
{
    if (ep_index > USBIP_EP_MAX) {
        return false;
    }
    bool first = b->ep_inflight[ep_index] == 0;
    bool multi = is_in && ep_index != 0;
    size_t new_charged = b->charged - charge(b->ep_bytes[ep_index]) + charge(b->ep_bytes[ep_index] + n);
    bool small = first && n <= USBIP_RESERVE_SMALL;
    // IN endpoints share the 32 KB, a first transfer included: an IN takes the large reserve only when it does not fit.
    bool shared = multi && b->inflight_bytes + n <= USBIP_INFLIGHT_BYTES && b->inflight_xfers < USBIP_INFLIGHT_XFERS;
    bool large = first && new_charged <= USBIP_INFLIGHT_HARD;

    if (!small && !shared && !large) {
        return false;
    }
    b->ep_inflight[ep_index]++;
    b->ep_bytes[ep_index] += n;
    b->charged = new_charged;
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
    size_t ep_bytes = b->ep_bytes[ep_index];
    size_t left = n >= ep_bytes || b->ep_inflight[ep_index] == 0 ? 0 : ep_bytes - n;

    b->charged = b->charged - charge(ep_bytes) + charge(left);
    b->ep_bytes[ep_index] = left;
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
