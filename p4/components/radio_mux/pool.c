#include "radio_mux_pool.h"

void radio_mux_pool_init(radio_mux_pool_t *p, uint8_t *slots, size_t slot_size, size_t n)
{
    if (n > RADIO_MUX_POOL_MAX_SLOTS) {
        n = RADIO_MUX_POOL_MAX_SLOTS;
    }
    p->slots = slots;
    p->slot_size = slot_size;
    p->n = n;
    p->free_mask = n == RADIO_MUX_POOL_MAX_SLOTS ? UINT32_MAX : (((uint32_t)1 << n) - 1);
    p->in_use = 0;
    p->drops = 0;
    p->bad_puts = 0;
}

uint8_t *radio_mux_pool_take(radio_mux_pool_t *p)
{
    if (p->free_mask == 0) {
        p->drops++;
        return NULL;
    }

    unsigned idx = (unsigned)__builtin_ctz(p->free_mask);

    p->free_mask &= ~((uint32_t)1 << idx);
    p->in_use++;
    return p->slots + (size_t)idx * p->slot_size;
}

void radio_mux_pool_put(radio_mux_pool_t *p, uint8_t *slot)
{
    uintptr_t at = (uintptr_t)slot, base = (uintptr_t)p->slots;

    if (slot == NULL || p->slot_size == 0 || at < base || at - base >= p->n * p->slot_size ||
            (at - base) % p->slot_size != 0) {
        p->bad_puts++;
        return;
    }

    uint32_t bit = (uint32_t)1 << ((at - base) / p->slot_size);

    if (p->free_mask & bit) {
        p->bad_puts++;
        return;
    }
    p->free_mask |= bit;
    p->in_use--;
}
