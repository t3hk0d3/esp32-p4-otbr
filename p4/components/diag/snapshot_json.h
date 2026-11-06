#pragma once

// GET /device/diag body (spec 5.12). Pure: host-tested in test_host/.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "lock_stats.h"
#include "windows.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t free_bytes;
    uint32_t min_free_bytes;
    uint32_t largest_free_block;
} diag_heap_info_t;

typedef struct {
    uint32_t uptime_s;
    const char *reset_reason;
    bool chip_temperature_valid; // false: the sensor has no reading, rendered as null
    float chip_temperature_c;
    const diag_view_t *view;
    diag_heap_info_t internal;
    diag_heap_info_t psram;
    const diag_lock_view_t *lock;
} diag_snapshot_input_t;

/** Write the body into `buf`. Returns its length, or -1 when `cap` is too small (nothing is truncated into a valid-looking body). */
int diag_snapshot_render(char *buf, size_t cap, const diag_snapshot_input_t *in);

/** Upper bound of a rendered body for DIAG_MAX_TASKS tasks with fully escaped names. */
#define DIAG_JSON_MAX_BYTES (20 * 1024)

#ifdef __cplusplus
}
#endif
