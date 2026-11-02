#include <stdlib.h>
#include <string.h>
#include "unity.h"
#include "unity_test_runner.h"
#include "windows.h"

#define TICK_US 1000u

static diag_windows_t *new_windows(void)
{
    diag_windows_t *w = malloc(sizeof(*w));

    diag_windows_init(w);
    return w;
}

static diag_task_sample_t task(const char *name, int core, uint32_t handle_id, uint32_t runtime)
{
    diag_task_sample_t t = {.core = core, .prio = 5, .state = 2, .runtime = runtime, .stack_hwm = 1000, .handle_id = handle_id};

    strncpy(t.name, name, sizeof(t.name) - 1);
    return t;
}

static void update(diag_windows_t *w, const diag_task_sample_t *tasks, size_t n, uint32_t total)
{
    const uint32_t totals[DIAG_CORES] = {total, total};

    diag_windows_update(w, tasks, n, totals);
}

static const diag_task_view_t *find(const diag_windows_t *w, const char *name)
{
    for (size_t i = 0; i < w->view.task_count; i++) {
        if (strcmp(w->view.tasks[i].task.name, name) == 0) {
            return &w->view.tasks[i];
        }
    }
    return NULL;
}

TEST_CASE("On each 1 s tick: updates per-task cpu_pct for the 1 s, 10 s and 60 s windows", "[diag]")
{
    diag_windows_t *w = new_windows();
    uint32_t runtime = 0;

    // 50 ticks at 50 %, 9 ticks at 10 %, the last tick at 100 % of core 0.
    for (uint32_t tick = 1; tick <= 60; tick++) {
        runtime += tick <= 50 ? 500 : (tick < 60 ? 100 : 1000);
        const diag_task_sample_t t = task("worker", 0, 1, runtime);
        update(w, &t, 1, tick * TICK_US);
    }
    const diag_task_view_t *v = find(w, "worker");

    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 100.0f, v->cpu_pct[0]);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 19.0f, v->cpu_pct[1]);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 44.83f, v->cpu_pct[2]);
    free(w);
}

TEST_CASE("On each 1 s tick: per-core load_pct equals 100 minus that core's idle-task share", "[diag]")
{
    diag_windows_t *w = new_windows();
    const diag_task_sample_t tasks[] = {
        task("IDLE0", 0, 1, 250),
        task("IDLE1", 1, 2, 900),
        task("busy", 1, 3, 100),
    };

    update(w, tasks, 3, TICK_US);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 75.0f, w->view.load_pct[0][0]);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 10.0f, w->view.load_pct[1][0]);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 75.0f, w->view.load_pct[0][2]);
    free(w);
}

TEST_CASE("On each 1 s tick: an unpinned task is measured against both cores", "[diag]")
{
    diag_windows_t *w = new_windows();
    const diag_task_sample_t t = task("floater", DIAG_CORE_ANY, 1, 500);

    update(w, &t, 1, TICK_US);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 25.0f, find(w, "floater")->cpu_pct[0]);
    free(w);
}

TEST_CASE("On runtime counter wrap: delta is computed modulo 2^32", "[diag]")
{
    diag_windows_t *w = new_windows();
    diag_task_sample_t t = task("worker", 0, 1, 0xFFFFFF00u);

    update(w, &t, 1, 0xFFFFFC18u);
    t.runtime = 0x00000100u; // 0x200 = 512 ticks later
    update(w, &t, 1, 0u);    // 1000 ticks later: the core total wrapped as well
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 51.2f, find(w, "worker")->cpu_pct[0]);
    free(w);
}

TEST_CASE("On a task's samples: adds every runtime delta to its cpu_us total", "[diag]")
{
    diag_windows_t *w = new_windows();
    diag_task_sample_t t = task("worker", 0, 1, 100);

    update(w, &t, 1, TICK_US);
    t.runtime = 350;
    update(w, &t, 1, 2 * TICK_US);
    TEST_ASSERT_EQUAL_UINT64(350, find(w, "worker")->cpu_us);
    free(w);
}

TEST_CASE("On runtime counter wrap: cpu_us keeps increasing by the modulo-2^32 delta", "[diag]")
{
    diag_windows_t *w = new_windows();
    diag_task_sample_t t = task("worker", 0, 1, 0xFFFFFF00u);

    update(w, &t, 1, 0xFFFFFC18u);
    t.runtime = 0x00000100u;
    update(w, &t, 1, 0u);
    TEST_ASSERT_EQUAL_UINT64(0xFFFFFF00ull + 0x200u, find(w, "worker")->cpu_us);
    free(w);
}

TEST_CASE("On a task replaced by a new one with the same name: the new cpu_us starts from its own runtime", "[diag]")
{
    diag_windows_t *w = new_windows();
    diag_task_sample_t t = task("worker", 0, 1, 5000);

    update(w, &t, 1, TICK_US);
    t.handle_id = 2;
    t.runtime = 40;
    update(w, &t, 1, 2 * TICK_US);
    TEST_ASSERT_EQUAL_UINT64(40, find(w, "worker")->cpu_us);
    free(w);
}

TEST_CASE("On each sample: adds the wall delta to wall_us and the core's idle-task delta to idle_us", "[diag]")
{
    diag_windows_t *w = new_windows();
    diag_task_sample_t tasks[2] = {task("IDLE0", 0, 1, 0), task("IDLE1", 1, 2, 0)};

    update(w, tasks, 2, 1000);
    tasks[0].runtime = 600;
    tasks[1].runtime = 900;
    update(w, tasks, 2, 2000);
    TEST_ASSERT_EQUAL_UINT64(2000, w->view.wall_us[0]);
    TEST_ASSERT_EQUAL_UINT64(2000, w->view.wall_us[1]);
    TEST_ASSERT_EQUAL_UINT64(600, w->view.idle_us[0]);
    TEST_ASSERT_EQUAL_UINT64(900, w->view.idle_us[1]);
    free(w);
}

TEST_CASE("On task created since the previous tick: includes it with cpu_pct computed from its runtime since creation", "[diag]")
{
    diag_windows_t *w = new_windows();
    diag_task_sample_t tasks[2] = {task("old", 0, 1, 100), task("new", 0, 2, 300)};

    update(w, tasks, 1, TICK_US);
    TEST_ASSERT_NULL(find(w, "new"));
    tasks[0].runtime = 200;
    update(w, tasks, 2, 2 * TICK_US);
    TEST_ASSERT_NOT_NULL(find(w, "new"));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 30.0f, find(w, "new")->cpu_pct[0]);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 10.0f, find(w, "old")->cpu_pct[0]);
    free(w);
}

TEST_CASE("On task deleted since the previous tick: removes it from the snapshot", "[diag]")
{
    diag_windows_t *w = new_windows();
    const diag_task_sample_t tasks[] = {task("keep", 0, 1, 100), task("gone", 0, 2, 100), task("also", 1, 3, 100)};

    update(w, tasks, 3, TICK_US);
    TEST_ASSERT_EQUAL(3, w->view.task_count);
    const diag_task_sample_t left[] = {tasks[0], tasks[2]};
    update(w, left, 2, 2 * TICK_US);
    TEST_ASSERT_EQUAL(2, w->view.task_count);
    TEST_ASSERT_NULL(find(w, "gone"));
    TEST_ASSERT_NOT_NULL(find(w, "keep"));
    TEST_ASSERT_NOT_NULL(find(w, "also"));
    free(w);
}

TEST_CASE("On a new task reusing a deleted one's slot: it starts from its own runtime", "[diag]")
{
    diag_windows_t *w = new_windows();
    diag_task_sample_t t = task("first", 0, 1, 900);

    update(w, &t, 1, TICK_US);
    t = task("second", 0, 2, 50); // different handle_id, smaller runtime than the old task had
    update(w, &t, 1, 2 * TICK_US);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 5.0f, find(w, "second")->cpu_pct[0]);
    free(w);
}

TEST_CASE("On more tasks than the window tracks: the extra ones are counted, not stored", "[diag]")
{
    diag_windows_t *w = new_windows();
    diag_task_sample_t tasks[DIAG_MAX_TASKS + 3];

    for (size_t i = 0; i < DIAG_MAX_TASKS + 3; i++) {
        tasks[i] = task("t", 0, (uint32_t)i + 1, 10);
    }
    update(w, tasks, DIAG_MAX_TASKS + 3, TICK_US);
    TEST_ASSERT_EQUAL(DIAG_MAX_TASKS, w->view.task_count);
    TEST_ASSERT_EQUAL(3, w->view.untracked);
    free(w);
}

TEST_CASE("On a task whose runtime delta exceeds its core's total: cpu_pct is clamped to 100", "[diag]")
{
    diag_windows_t *w = new_windows();
    diag_task_sample_t tasks[] = {task("busy", 0, 1, 0)};

    update(w, tasks, 1, 0);
    tasks[0].runtime = 5 * TICK_US; // more than the core's total of this tick (clock skew between counters)
    update(w, tasks, 1, TICK_US);
    for (int win = 0; win < DIAG_WINDOW_COUNT; win++) {
        TEST_ASSERT_EQUAL_FLOAT(100.0f, find(w, "busy")->cpu_pct[win]);
    }
    free(w);
}

TEST_CASE("On an idle task above its core's total: load_pct is clamped to 0, never negative", "[diag]")
{
    diag_windows_t *w = new_windows();
    diag_task_sample_t tasks[] = {task("IDLE0", 0, 1, 0)};

    update(w, tasks, 1, 0);
    tasks[0].runtime = 3 * TICK_US;
    update(w, tasks, 1, TICK_US);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, w->view.load_pct[0][0]);
    free(w);
}

TEST_CASE("On a tick where the core total did not advance: cpu_pct and load_pct are 0, not NaN or infinite", "[diag]")
{
    diag_windows_t *w = new_windows();
    diag_task_sample_t tasks[] = {task("worker", 1, 1, 0), task("IDLE1", 1, 2, 0), task("any", -1, 3, 0)};

    update(w, tasks, 3, 0);
    tasks[0].runtime = 500;
    tasks[1].runtime = 500;
    tasks[2].runtime = 500;
    update(w, tasks, 3, 0); // total unchanged
    for (int win = 0; win < DIAG_WINDOW_COUNT; win++) {
        TEST_ASSERT_EQUAL_FLOAT(0.0f, find(w, "worker")->cpu_pct[win]);
        TEST_ASSERT_EQUAL_FLOAT(0.0f, find(w, "any")->cpu_pct[win]);
        TEST_ASSERT_EQUAL_FLOAT(0.0f, find(w, "IDLE1")->cpu_pct[win]);
    }
    TEST_ASSERT_EQUAL_FLOAT(100.0f, w->view.load_pct[1][0]);
    free(w);
}
