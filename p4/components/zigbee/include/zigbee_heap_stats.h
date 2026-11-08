#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Byte accounting for the allocator shim that serves ziggurat. Not thread-safe: the caller serialises access. */
typedef struct {
    size_t current;
    size_t peak;
    uint32_t failures;
} zigbee_heap_stats_t;

/** Counts a successful allocation of `size` bytes. */
void zigbee_heap_stats_alloc(zigbee_heap_stats_t *st, size_t size);

/** Counts a free of `size` bytes. `current` never goes below 0. */
void zigbee_heap_stats_free(zigbee_heap_stats_t *st, size_t size);

/** Counts a successful resize from `old_size` to `new_size` bytes (0 for either means a plain alloc or free). */
void zigbee_heap_stats_realloc(zigbee_heap_stats_t *st, size_t old_size, size_t new_size);

/** Counts an allocation or resize that returned NULL. Leaves `current` and `peak` alone. */
void zigbee_heap_stats_failed(zigbee_heap_stats_t *st);

#ifdef __cplusplus
}
#endif
