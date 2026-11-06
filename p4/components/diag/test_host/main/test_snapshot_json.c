#include <stdlib.h>
#include <string.h>
#include "unity.h"
#include "unity_test_runner.h"
#include "snapshot_json.h"

static diag_snapshot_input_t sample_input(diag_view_t *view, diag_lock_view_t *lock)
{
    memset(view, 0, sizeof(*view));
    memset(lock, 0, sizeof(*lock));
    view->task_count = 2;
    strcpy(view->tasks[0].task.name, "IDLE0");
    view->tasks[0].task.core = 0;
    view->tasks[0].task.state = 0;
    view->tasks[0].cpu_pct[0] = 12.34f;
    strcpy(view->tasks[1].task.name, "ev\"il\n");
    view->tasks[1].task.core = DIAG_CORE_ANY;
    view->tasks[1].task.state = 2;
    view->tasks[1].task.prio = 7;
    view->tasks[1].task.stack_hwm = 1234;
    view->load_pct[1][0] = 99.96f;
    lock->since_boot.acquisitions = 5;
    lock->since_boot.max_hold_us = 321;
    strcpy(lock->since_boot.hold_task, "ot");
    return (diag_snapshot_input_t) {
        .uptime_s = 42, .reset_reason = "power-on", .view = view,
        .internal = {.free_bytes = 1, .min_free_bytes = 2, .largest_free_block = 3},
        .psram = {.free_bytes = 4, .min_free_bytes = 5, .largest_free_block = 6},
        .lock = lock,
    };
}

TEST_CASE("On render: writes the spec sections with percentages to one decimal", "[diag]")
{
    diag_view_t *view = malloc(sizeof(*view));
    diag_lock_view_t lock;
    const diag_snapshot_input_t in = sample_input(view, &lock);
    char buf[4096];

    TEST_ASSERT_GREATER_THAN(0, diag_snapshot_render(buf, sizeof(buf), &in));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"uptime_s\":42,\"reset_reason\":\"power-on\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "{\"core\":1,\"load_pct\":{\"1s\":100.0,\"10s\":0.0,\"60s\":0.0}}"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"name\":\"IDLE0\",\"core\":0,\"priority\":0,\"state\":\"running\",\"cpu_pct\":{\"1s\":12.3,"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"core\":\"any\",\"priority\":7,\"state\":\"blocked\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"stack_free_min_bytes\":1234"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"memory\":{\"internal\":{\"free\":1,\"min_free\":2,\"largest_free_block\":3},\"psram\":{\"free\":4,"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"ot_lock\":{\"acquisitions\":5,\"max_hold_us\":321,\"max_wait_us\":0,\"longest_holder\":\"ot\",\"last_60s\":{"));
    free(view);
}

TEST_CASE("On a task name with quotes and control characters: it is escaped", "[diag]")
{
    diag_view_t *view = malloc(sizeof(*view));
    diag_lock_view_t lock;
    const diag_snapshot_input_t in = sample_input(view, &lock);
    char buf[4096];

    TEST_ASSERT_GREATER_THAN(0, diag_snapshot_render(buf, sizeof(buf), &in));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"name\":\"ev\\\"il\\n\""));
    free(view);
}

TEST_CASE("On a buffer that is too small: returns -1", "[diag]")
{
    diag_view_t *view = malloc(sizeof(*view));
    diag_lock_view_t lock;
    const diag_snapshot_input_t in = sample_input(view, &lock);
    char buf[64];

    TEST_ASSERT_EQUAL(-1, diag_snapshot_render(buf, sizeof(buf), &in));
    free(view);
}

TEST_CASE("On a full window of tasks with worst-case names: fits DIAG_JSON_MAX_BYTES", "[diag]")
{
    diag_view_t *view = malloc(sizeof(*view));
    diag_lock_view_t lock;
    diag_snapshot_input_t in = sample_input(view, &lock);
    char *buf = malloc(DIAG_JSON_MAX_BYTES);

    view->task_count = DIAG_MAX_TASKS;
    for (size_t i = 0; i < DIAG_MAX_TASKS; i++) {
        memset(view->tasks[i].task.name, 0x01, DIAG_NAME_MAX - 1); // each byte escapes to \u0001
        view->tasks[i].task.name[DIAG_NAME_MAX - 1] = '\0';
        view->tasks[i].task.prio = 4294967295u;
        view->tasks[i].task.stack_hwm = 4294967295u;
        for (int win = 0; win < DIAG_WINDOW_COUNT; win++) {
            view->tasks[i].cpu_pct[win] = 100.0f;
        }
    }
    in.uptime_s = 4294967295u;
    TEST_ASSERT_GREATER_THAN(0, diag_snapshot_render(buf, DIAG_JSON_MAX_BYTES, &in));
    free(buf);
    free(view);
}

static void render_with_temperature(char *buf, size_t cap, bool valid, float celsius)
{
    diag_view_t *view = malloc(sizeof(*view));
    diag_lock_view_t lock;
    diag_snapshot_input_t in = sample_input(view, &lock);

    in.chip_temperature_valid = valid;
    in.chip_temperature_c = celsius;
    TEST_ASSERT_GREATER_THAN(0, diag_snapshot_render(buf, cap, &in));
    free(view);
}

TEST_CASE("On a valid temperature: writes chip_temperature_c after reset_reason with one decimal", "[diag]")
{
    char buf[4096];

    render_with_temperature(buf, sizeof(buf), true, 47.25f);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"reset_reason\":\"power-on\",\"chip_temperature_c\":47.3,\"cpu\":["));
}

TEST_CASE("On a temperature below zero: writes the sign once", "[diag]")
{
    char buf[4096];

    render_with_temperature(buf, sizeof(buf), true, -0.5f);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"chip_temperature_c\":-0.5,"));
    render_with_temperature(buf, sizeof(buf), true, -3.04f);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"chip_temperature_c\":-3.0,"));
    render_with_temperature(buf, sizeof(buf), true, -3.06f);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"chip_temperature_c\":-3.1,"));
}

TEST_CASE("On a temperature that rounds to zero from below: writes 0.0, not -0.0", "[diag]")
{
    char buf[4096];

    render_with_temperature(buf, sizeof(buf), true, -0.04f);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"chip_temperature_c\":0.0,"));
}

TEST_CASE("On an invalid temperature: writes chip_temperature_c null", "[diag]")
{
    char buf[4096];

    render_with_temperature(buf, sizeof(buf), false, 47.0f);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"reset_reason\":\"power-on\",\"chip_temperature_c\":null,\"cpu\":["));
}

void app_main(void)
{
    UNITY_BEGIN();
    unity_run_all_tests();
    int failures = (int)UNITY_END();
    exit(failures == 0 ? 0 : 1);
}
