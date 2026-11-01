#pragma once

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

#ifdef __cplusplus
}
#endif
