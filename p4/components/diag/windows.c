#include "windows.h"

#include <string.h>

const uint32_t diag_window_slots[DIAG_WINDOW_COUNT] = {1, 10, 60};

static bool is_idle(const diag_task_sample_t *t)
{
    return strncmp(t->name, "IDLE", 4) == 0 && t->core >= 0 && t->core < DIAG_CORES;
}

void diag_windows_init(diag_windows_t *w)
{
    memset(w, 0, sizeof(*w));
}

static size_t find_track(const diag_windows_t *w, uint32_t handle_id)
{
    for (size_t i = 0; i < w->view.task_count; i++) {
        if (w->view.tasks[i].task.handle_id == handle_id) {
            return i;
        }
    }
    return DIAG_MAX_TASKS;
}

// Sum of the newest `slots` entries of a ring whose latest slot is `head`.
static uint64_t ring_sum(const uint32_t *ring, size_t head, uint32_t slots)
{
    uint64_t sum = 0;

    for (uint32_t i = 0; i < slots; i++) {
        sum += ring[(head + DIAG_WINDOW_SLOTS - i) % DIAG_WINDOW_SLOTS];
    }
    return sum;
}

static float pct(uint64_t part, uint64_t whole)
{
    if (whole == 0) {
        return 0.0f;
    }
    const float value = 100.0f * (float)part / (float)whole;
    return value > 100.0f ? 100.0f : value;
}

// Drop entry `i` by moving the last entry into its place.
static void drop_task(diag_windows_t *w, size_t i)
{
    const size_t last = w->view.task_count - 1;

    if (i != last) {
        w->view.tasks[i] = w->view.tasks[last];
        w->track[i] = w->track[last];
    }
    memset(&w->track[last], 0, sizeof(w->track[last]));
    w->view.task_count = last;
}

void diag_windows_update(diag_windows_t *w, const diag_task_sample_t *tasks, size_t n, const uint32_t total_runtime_per_core[DIAG_CORES])
{
    bool seen[DIAG_MAX_TASKS] = {false};

    w->head = (w->head + 1) % DIAG_WINDOW_SLOTS;
    if (w->samples < DIAG_WINDOW_SLOTS) {
        w->samples++;
    }
    for (int c = 0; c < DIAG_CORES; c++) {
        w->core_delta[c][w->head] = total_runtime_per_core[c] - w->prev_total[c];
        w->prev_total[c] = total_runtime_per_core[c];
        w->view.wall_us[c] += w->core_delta[c][w->head];
    }

    // Tasks of this sample: update known ones, add new ones.
    w->view.untracked = 0;
    for (size_t k = 0; k < n; k++) {
        size_t i = find_track(w, tasks[k].handle_id);

        if (i == DIAG_MAX_TASKS) {
            if (w->view.task_count == DIAG_MAX_TASKS) {
                w->view.untracked++;
                continue;
            }
            i = w->view.task_count++;
            memset(&w->track[i], 0, sizeof(w->track[i]));
            w->track[i].used = true; // prev_runtime 0: the first delta is the whole runtime
            w->view.tasks[i].cpu_us = 0;
        }
        w->view.tasks[i].task = tasks[k];
        w->track[i].delta[w->head] = tasks[k].runtime - w->track[i].prev_runtime;
        w->view.tasks[i].cpu_us += w->track[i].delta[w->head];
        if (is_idle(&tasks[k])) {
            w->view.idle_us[tasks[k].core] += w->track[i].delta[w->head];
        }
        w->track[i].prev_runtime = tasks[k].runtime;
        seen[i] = true;
    }

    // Drop what vanished (walk backwards: drop_task moves the last entry down, which was already visited).
    for (size_t i = w->view.task_count; i-- > 0;) {
        if (!seen[i]) {
            drop_task(w, i);
        }
    }

    for (size_t i = 0; i < w->view.task_count; i++) {
        diag_task_view_t *t = &w->view.tasks[i];

        for (int win = 0; win < DIAG_WINDOW_COUNT; win++) {
            const uint32_t slots = diag_window_slots[win] < w->samples ? diag_window_slots[win] : w->samples;
            uint64_t whole;

            if (t->task.core >= 0 && t->task.core < DIAG_CORES) {
                whole = ring_sum(w->core_delta[t->task.core], w->head, slots);
            } else {
                whole = ring_sum(w->core_delta[0], w->head, slots) + ring_sum(w->core_delta[1], w->head, slots);
            }
            t->cpu_pct[win] = pct(ring_sum(w->track[i].delta, w->head, slots), whole);
        }
    }

    for (int c = 0; c < DIAG_CORES; c++) {
        for (int win = 0; win < DIAG_WINDOW_COUNT; win++) {
            w->view.load_pct[c][win] = 0.0f;
        }
    }
    for (size_t i = 0; i < w->view.task_count; i++) {
        const diag_task_view_t *t = &w->view.tasks[i];

        if (is_idle(&t->task)) {
            for (int win = 0; win < DIAG_WINDOW_COUNT; win++) {
                w->view.load_pct[t->task.core][win] = 100.0f - t->cpu_pct[win];
            }
        }
    }
}
