// Frame pool accounting: fixed slots handed out and taken back. Pure: no ESP-IDF calls. The board code allocates the
// slots (DMA-capable, cache-line aligned) and adds a spinlock around take and put.
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RADIO_MUX_POOL_MAX_SLOTS 32

typedef struct {
    uint8_t *slots;
    size_t slot_size, n;
    uint32_t free_mask; // bit i set = slot i free
    uint32_t in_use, drops;
    uint32_t bad_puts; // puts of a slot already free or of a pointer that is no slot start of this pool
} radio_mux_pool_t;

// `slots` holds n slots of slot_size bytes; n is capped at RADIO_MUX_POOL_MAX_SLOTS. All slots start free.
void radio_mux_pool_init(radio_mux_pool_t *p, uint8_t *slots, size_t slot_size, size_t n);

// The lowest free slot, or NULL when none is (drops + 1).
uint8_t *radio_mux_pool_take(radio_mux_pool_t *p);

// Gives a slot back. A slot that is already free, NULL or a pointer that is no slot start of this pool changes
// nothing but bad_puts.
void radio_mux_pool_put(radio_mux_pool_t *p, uint8_t *slot);

#ifdef __cplusplus
}
#endif
