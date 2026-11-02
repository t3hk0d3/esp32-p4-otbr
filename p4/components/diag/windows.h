#pragma once

// Pure window arithmetic of the diag sampler (no FreeRTOS): host-tested in test_host/.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "diag.h" // DIAG_NAME_MAX, DIAG_MAX_TASKS, DIAG_CORES

#ifdef __cplusplus
extern "C" {
#endif

#define DIAG_WINDOW_SLOTS 60 /**< one slot per sampler tick */
#define DIAG_WINDOW_COUNT 3  /**< 1 s, 10 s, 60 s */
#define DIAG_CORE_ANY (-1)   /**< task_sample.core of an unpinned task */

/** Slots summed by window index 0, 1, 2. */
extern const uint32_t diag_window_slots[DIAG_WINDOW_COUNT];

typedef struct {
    char name[DIAG_NAME_MAX];
    int core;           /**< pinned core, DIAG_CORE_ANY when unpinned */
    uint32_t prio;
    uint32_t state;     /**< eTaskState: 0 running, 1 ready, 2 blocked, 3 suspended, 4 deleted, 5 invalid */
    uint32_t runtime;   /**< cumulative run time counter, wraps at 2^32 */
    uint32_t stack_hwm; /**< stack high-water mark, bytes still free at the minimum */
    uint32_t handle_id; /**< unique per task instance (the kernel's task number), never reused while the task lives */
} diag_task_sample_t;

typedef struct {
    diag_task_sample_t task;
    float cpu_pct[DIAG_WINDOW_COUNT]; /**< share of its core (both cores for an unpinned task) */
    uint64_t cpu_us;                  /**< run time since the task was first seen, summed from the modulo-2^32 deltas */
} diag_task_view_t;

/** What a request reads: refreshed by every diag_windows_update. */
typedef struct {
    size_t task_count;
    uint32_t untracked; /**< tasks of the last sample beyond DIAG_MAX_TASKS */
    diag_task_view_t tasks[DIAG_MAX_TASKS];
    float load_pct[DIAG_CORES][DIAG_WINDOW_COUNT]; /**< 100 minus the share of that core's idle task */
    uint64_t wall_us[DIAG_CORES]; /**< sum of the core totals' deltas since the first sample */
    uint64_t idle_us[DIAG_CORES]; /**< sum of that core's idle-task deltas since the first sample */
} diag_view_t;

typedef struct {
    bool used;
    uint32_t prev_runtime;
    uint32_t delta[DIAG_WINDOW_SLOTS];
} diag_task_track_t;

typedef struct {
    uint32_t samples;      /**< updates so far, saturating at DIAG_WINDOW_SLOTS */
    size_t head;           /**< slot written by the latest update */
    uint32_t prev_total[DIAG_CORES];
    uint32_t core_delta[DIAG_CORES][DIAG_WINDOW_SLOTS];
    diag_task_track_t track[DIAG_MAX_TASKS]; /**< parallel to view.tasks */
    diag_view_t view;
} diag_windows_t;

void diag_windows_init(diag_windows_t *w);

/**
 * Take one sample. `tasks` are the live tasks (cumulative runtimes), `total_runtime_per_core` the cumulative run time
 * counter of each core (the same wall clock for both). Deltas are modulo 2^32. A task seen for the first time counts its
 * whole runtime as the delta; tasks missing from `tasks` are dropped.
 */
void diag_windows_update(diag_windows_t *w, const diag_task_sample_t *tasks, size_t n, const uint32_t total_runtime_per_core[DIAG_CORES]);

#ifdef __cplusplus
}
#endif
