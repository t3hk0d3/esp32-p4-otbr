#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Start the sampler task (core 1, lowest application priority, 1 s period). It reads uxTaskGetSystemState once per
 * second into rolling 1 s / 10 s / 60 s windows; nothing else ever walks the task list. ESP_ERR_INVALID_STATE if started
 * already, ESP_ERR_NO_MEM when memory or the task cannot be created (everything allocated is released then, a later call
 * may retry). Diagnostics are not essential: a failure must not stop the boot, and until a start succeeded
 * diag_snapshot_json answers ESP_ERR_INVALID_STATE (GET /device/diag 503 "not_started"). Tasks beyond DIAG_MAX_TASKS are
 * left out of the snapshot and logged once.
 *
 * The OpenThread lock statistics (spec 5.12) are collected from boot by link-time wraps of esp_openthread_lock_acquire /
 * release, which this component's INTERFACE link options enable: they cover everything that takes the OpenThread lock
 * through those functions (libopenthread callers, br_core, the Thread REST server, esp_openthread itself).
 * The prebuilt libopenthread_br.a (the border router internals) only calls esp_openthread_task_switching_lock_*, which take
 * the task-switching mutex and are not wrapped: holds by those paths are not counted as holds; they show up only as wait
 * time (max_wait_us) of the wrapped callers.
 */
esp_err_t diag_start(void);

/**
 * GET /device/diag body (spec 5.12) in a heap buffer the caller frees. Copies the precomputed snapshot under a mutex
 * (waits at most 100 ms, then ESP_ERR_TIMEOUT) and never takes the OpenThread lock or walks the task list.
 * ESP_ERR_INVALID_STATE before diag_start, ESP_ERR_NO_MEM, ESP_ERR_INVALID_ARG for NULL.
 */
esp_err_t diag_snapshot_json(char **out_json);

#define DIAG_METRICS_CORES 2
#define DIAG_METRICS_MAX_TASKS 48 /**< equals the sampler's DIAG_MAX_TASKS */
#define DIAG_METRICS_NAME_MAX 16

/** Cumulative figures for GET /metrics (totals since boot, not windows). */
typedef struct {
    uint32_t uptime_s;
    const char *reset_reason; /**< static string */
    struct {
        uint64_t wall_us; /**< run time counter total of the core */
        uint64_t idle_us; /**< of that, the core's idle task: busy = wall - idle */
    } cores[DIAG_METRICS_CORES];
    size_t task_count;
    struct {
        char name[DIAG_METRICS_NAME_MAX];
        uint64_t cpu_us;
        uint32_t stack_free_min; /**< bytes */
    } tasks[DIAG_METRICS_MAX_TASKS];
    uint32_t untracked;
    struct {
        uint32_t free;
        uint32_t min_free;
        uint32_t largest_free_block;
    } internal, psram;
    struct {
        uint32_t acquisitions;
        uint64_t hold_us_total;
        uint64_t wait_us_total;
        uint32_t max_hold_us;     /**< since boot */
        uint32_t max_wait_us;     /**< since boot */
        uint32_t max_hold_60s_us;
        uint32_t max_wait_60s_us;
    } lock;
} diag_metrics_view_t;

/**
 * Copy the cumulative figures for GET /metrics. Takes the snapshot mutex (waits at most 100 ms, then ESP_ERR_TIMEOUT)
 * and never takes the OpenThread lock. ESP_ERR_INVALID_STATE before diag_start, ESP_ERR_INVALID_ARG for NULL.
 */
esp_err_t diag_metrics_view(diag_metrics_view_t *out);

#ifdef __cplusplus
}
#endif
