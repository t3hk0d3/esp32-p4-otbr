#pragma once

// Pure bookkeeping of the OpenThread lock statistics (no FreeRTOS): host-tested in test_host/. The caller serialises
// access (ot_lock_wrap.c uses a spinlock) and supplies the clock and the task name.

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DIAG_LOCK_BUCKETS 60 /**< one per second, for the last-60-s figures */
#define DIAG_LOCK_NAME_MAX 16

typedef struct {
    uint32_t sec; /**< the second this bucket holds */
    uint32_t acquisitions;
    uint32_t max_hold_us;
    uint32_t max_wait_us;
    char hold_task[DIAG_LOCK_NAME_MAX];
} diag_lock_bucket_t;

typedef struct {
    uint32_t depth;         /**< recursion depth of the current holder, 0 = free */
    uint64_t hold_start_us; /**< when the outermost acquire succeeded */
    uint32_t acquisitions;  /**< outermost acquires since boot */
    uint64_t hold_us_total; /**< sum of the outermost holds since boot */
    uint64_t wait_us_total; /**< sum of every acquire's wait since boot, timeouts included */
    uint32_t max_hold_us;
    uint32_t max_wait_us;
    char hold_task[DIAG_LOCK_NAME_MAX]; /**< the task of the longest hold */
    diag_lock_bucket_t buckets[DIAG_LOCK_BUCKETS];
} diag_lock_stats_t;

typedef struct {
    uint32_t acquisitions;
    uint32_t max_hold_us;
    uint32_t max_wait_us;
    char hold_task[DIAG_LOCK_NAME_MAX];
} diag_lock_totals_t;

typedef struct {
    diag_lock_totals_t since_boot;
    diag_lock_totals_t last_60s;
    uint64_t hold_us_total; /**< since boot */
    uint64_t wait_us_total; /**< since boot */
} diag_lock_view_t;

/**
 * An acquire attempt finished at `now_us` after waiting `wait_us`. `acquired` false (timeout) only records the wait.
 * A nested acquire by the holder only deepens the recursion: it neither counts nor starts a hold.
 */
void diag_lock_on_acquire(diag_lock_stats_t *s, uint64_t now_us, uint32_t wait_us, bool acquired);

/** The holder released once. Only the release that ends the outermost hold records the hold time, for `task`. A release of a free lock is ignored. */
void diag_lock_on_release(diag_lock_stats_t *s, uint64_t now_us, const char *task);

/** Figures since boot and over the seconds (now_us / 1e6 - 59) .. (now_us / 1e6). */
void diag_lock_view(const diag_lock_stats_t *s, uint64_t now_us, diag_lock_view_t *out);

#ifdef __cplusplus
}
#endif
