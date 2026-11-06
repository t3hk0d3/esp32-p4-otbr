// Sampler task and snapshot access (spec 5.12). The only caller of uxTaskGetSystemState is the sampler task; a request
// copies the finished view under a mutex.
#include "diag.h"

#include <stdlib.h>
#include <string.h>

#include "board_cores.h"
#include "driver/temperature_sensor.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "diag_priv.h"
#include "snapshot_json.h"
#include "windows.h"

static const char *TAG = "diag";

#define SAMPLER_CORE MGMT_CORE
#define SAMPLER_PRIORITY (tskIDLE_PRIORITY + 1) // lowest application priority
#define SAMPLER_STACK 4096
#define SAMPLER_PERIOD_MS 1000
#define TASK_HEADROOM 8      // tasks that may appear between uxTaskGetNumberOfTasks and uxTaskGetSystemState
#define MUTEX_WAIT_MS 100
#define TSENS_RANGE_MIN_C 20 // start range only: the driver re-picks the range from the reading
#define TSENS_RANGE_MAX_C 100

static SemaphoreHandle_t s_mutex;
static diag_windows_t *s_windows; // guarded by s_mutex
static diag_heap_info_t s_internal; // guarded by s_mutex
static diag_heap_info_t s_psram;
static bool s_chip_temperature_valid; // guarded by s_mutex
static float s_chip_temperature_c;
static temperature_sensor_handle_t s_tsens; // NULL: the sensor is unavailable
static bool s_started;
static bool s_untracked_logged; // sampler task only

static diag_heap_info_t read_heap(uint32_t caps)
{
    multi_heap_info_t info;

    heap_caps_get_info(&info, caps);
    return (diag_heap_info_t) {
        .free_bytes = (uint32_t)info.total_free_bytes,
        .min_free_bytes = (uint32_t)info.minimum_free_bytes,
        .largest_free_block = (uint32_t)info.largest_free_block,
    };
}

static void to_sample(const TaskStatus_t *in, diag_task_sample_t *out)
{
    memset(out, 0, sizeof(*out));
    strncpy(out->name, in->pcTaskName ? in->pcTaskName : "", sizeof(out->name) - 1);
    out->core = (in->xCoreID == 0 || in->xCoreID == 1) ? (int)in->xCoreID : DIAG_CORE_ANY;
    out->prio = (uint32_t)in->uxCurrentPriority;
    out->state = (uint32_t)in->eCurrentState;
    out->runtime = (uint32_t)in->ulRunTimeCounter; // the counter may be 64-bit: deltas are taken modulo 2^32
    out->stack_hwm = (uint32_t)in->usStackHighWaterMark;
    out->handle_id = (uint32_t)in->xTaskNumber; // unique per task instance, unlike the handle's address
}

// Buffers of the sampler, sized from the live task count plus headroom and never beyond what the windows track.
typedef struct {
    TaskStatus_t *status;
    diag_task_sample_t *samples;
    UBaseType_t cap;
} sample_buffers_t;

static bool buffers_ensure(sample_buffers_t *b, UBaseType_t want)
{
    const UBaseType_t limit = 2 * DIAG_MAX_TASKS; // beyond DIAG_MAX_TASKS the windows only count the rest

    if (want > limit) {
        want = limit;
    }
    if (want <= b->cap) {
        return true;
    }
    TaskStatus_t *status = realloc(b->status, want * sizeof(*status));
    if (status) {
        b->status = status;
    }
    diag_task_sample_t *samples = realloc(b->samples, want * sizeof(*samples));
    if (samples) {
        b->samples = samples;
    }
    if (!status || !samples) {
        // one array may have grown and the other not: keep the capacity of the smaller one
        return false;
    }
    b->cap = want;
    return true;
}

static void sample_once(sample_buffers_t *b)
{
    if (!buffers_ensure(b, uxTaskGetNumberOfTasks() + TASK_HEADROOM)) {
        ESP_LOGW(TAG, "no memory for the task array, skipping a sample");
        return;
    }
    uint32_t total = 0;
    const UBaseType_t n = uxTaskGetSystemState(b->status, b->cap, &total);

    if (n == 0) { // more tasks than the array holds: it grows on the next tick
        const bool at_limit = b->cap >= 2 * DIAG_MAX_TASKS; // the array cannot grow any more

        buffers_ensure(b, b->cap + TASK_HEADROOM);
        if (at_limit && !s_untracked_logged) {
            s_untracked_logged = true;
            ESP_LOGW(TAG, "more than %d tasks: the sampler cannot read them, /device/diag is not updated", 2 * DIAG_MAX_TASKS);
        }
        return;
    }
    for (UBaseType_t i = 0; i < n; i++) {
        to_sample(&b->status[i], &b->samples[i]);
    }
    const uint32_t totals[DIAG_CORES] = {total, total}; // both cores run on the same wall clock
    const diag_heap_info_t internal = read_heap(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const diag_heap_info_t psram = read_heap(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    float chip_c = 0.0f;
    const bool chip_valid = s_tsens && temperature_sensor_get_celsius(s_tsens, &chip_c) == ESP_OK;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    diag_windows_update(s_windows, b->samples, n, totals);
    s_internal = internal;
    s_psram = psram;
    s_chip_temperature_valid = chip_valid;
    s_chip_temperature_c = chip_c;
    const uint32_t untracked = s_windows->view.untracked;

    xSemaphoreGive(s_mutex);
    if (untracked > 0 && !s_untracked_logged) {
        s_untracked_logged = true; // once: the figure stays in the snapshot (view.untracked)
        ESP_LOGW(TAG, "%u tasks beyond the %d the sampler tracks are left out of /device/diag", (unsigned)untracked, DIAG_MAX_TASKS);
    }
}

static void sampler_task(void *arg)
{
    sample_buffers_t buffers = {0};
    TickType_t last_wake = xTaskGetTickCount();

    for (;;) {
        sample_once(&buffers);
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(SAMPLER_PERIOD_MS));
    }
}

// Nothing is left behind by a start that failed half-way, so that it can be retried; snapshots answer "not started"
// (diag_snapshot_json checks s_started) until the sampler task exists.
static void release_resources(void)
{
    if (s_mutex) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
    }
    heap_caps_free(s_windows);
    s_windows = NULL;
}

// Not essential: without the sensor the temperature stays unavailable and the rest of the diagnostics run.
static void start_temperature_sensor(void)
{
    temperature_sensor_config_t config = TEMPERATURE_SENSOR_CONFIG_DEFAULT(TSENS_RANGE_MIN_C, TSENS_RANGE_MAX_C);
    esp_err_t err = temperature_sensor_install(&config, &s_tsens);

    if (err == ESP_OK) {
        err = temperature_sensor_enable(s_tsens);
        if (err != ESP_OK) {
            temperature_sensor_uninstall(s_tsens);
        }
    }
    if (err != ESP_OK) {
        s_tsens = NULL;
        ESP_LOGW(TAG, "chip temperature sensor unavailable: %s", esp_err_to_name(err));
    }
}

static void stop_temperature_sensor(void)
{
    if (s_tsens) {
        temperature_sensor_disable(s_tsens);
        temperature_sensor_uninstall(s_tsens);
        s_tsens = NULL;
    }
}

esp_err_t diag_start(void)
{
    ESP_RETURN_ON_FALSE(!s_started, ESP_ERR_INVALID_STATE, TAG, "already started");
    // The windows are large and only read once a second: PSRAM first, internal RAM if there is none.
    s_windows = heap_caps_calloc(1, sizeof(*s_windows), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_windows) {
        s_windows = heap_caps_calloc(1, sizeof(*s_windows), MALLOC_CAP_8BIT);
    }
    s_mutex = xSemaphoreCreateMutex();
    if (!s_windows || !s_mutex) {
        release_resources();
        ESP_LOGE(TAG, "no memory for the diagnostics");
        return ESP_ERR_NO_MEM;
    }
    diag_windows_init(s_windows);
    start_temperature_sensor();
    if (xTaskCreatePinnedToCore(sampler_task, "diag", SAMPLER_STACK, NULL, SAMPLER_PRIORITY, NULL, SAMPLER_CORE) != pdPASS) {
        release_resources();
        stop_temperature_sensor();
        ESP_LOGE(TAG, "cannot create the sampler task");
        return ESP_ERR_NO_MEM;
    }
    s_started = true;
    return ESP_OK;
}

static const char *reset_reason_name(esp_reset_reason_t reason)
{
    switch (reason) {
    case ESP_RST_POWERON:
        return "power-on";
    case ESP_RST_EXT:
        return "external";
    case ESP_RST_SW:
        return "software";
    case ESP_RST_PANIC:
        return "panic";
    case ESP_RST_INT_WDT:
        return "interrupt watchdog";
    case ESP_RST_TASK_WDT:
        return "task watchdog";
    case ESP_RST_WDT:
        return "other watchdog";
    case ESP_RST_DEEPSLEEP:
        return "deep sleep";
    case ESP_RST_BROWNOUT:
        return "brownout";
    case ESP_RST_SDIO:
        return "sdio";
    default:
        return "unknown";
    }
}

esp_err_t diag_snapshot_json(char **out_json)
{
    ESP_RETURN_ON_FALSE(out_json != NULL, ESP_ERR_INVALID_ARG, TAG, "no output");
    *out_json = NULL;
    ESP_RETURN_ON_FALSE(s_started, ESP_ERR_INVALID_STATE, TAG, "not started");

    diag_view_t *view = heap_caps_malloc(sizeof(*view), MALLOC_CAP_8BIT);
    char *json = malloc(DIAG_JSON_MAX_BYTES);
    if (!view || !json) {
        free(view);
        free(json);
        return ESP_ERR_NO_MEM;
    }
    diag_snapshot_input_t in = {.view = view};

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(MUTEX_WAIT_MS)) != pdTRUE) {
        free(view);
        free(json);
        return ESP_ERR_TIMEOUT;
    }
    *view = s_windows->view;
    in.internal = s_internal;
    in.psram = s_psram;
    in.chip_temperature_valid = s_chip_temperature_valid;
    in.chip_temperature_c = s_chip_temperature_c;
    xSemaphoreGive(s_mutex);

    diag_lock_view_t lock;

    diag_ot_lock_view(&lock);
    in.lock = &lock;
    in.uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);
    in.reset_reason = reset_reason_name(esp_reset_reason());
    const int len = diag_snapshot_render(json, DIAG_JSON_MAX_BYTES, &in);

    free(view);
    if (len < 0) {
        free(json);
        return ESP_ERR_INVALID_SIZE;
    }
    *out_json = json;
    return ESP_OK;
}

esp_err_t diag_metrics_view(diag_metrics_view_t *out)
{
    ESP_RETURN_ON_FALSE(out != NULL, ESP_ERR_INVALID_ARG, TAG, "no output");
    ESP_RETURN_ON_FALSE(s_started, ESP_ERR_INVALID_STATE, TAG, "not started");
    memset(out, 0, sizeof(*out));
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(MUTEX_WAIT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    const diag_view_t *view = &s_windows->view;

    for (int c = 0; c < DIAG_CORES; c++) {
        out->cores[c].wall_us = view->wall_us[c];
        out->cores[c].idle_us = view->idle_us[c];
    }
    out->task_count = view->task_count;
    for (size_t i = 0; i < view->task_count; i++) {
        memcpy(out->tasks[i].name, view->tasks[i].task.name, sizeof(out->tasks[i].name));
        out->tasks[i].cpu_us = view->tasks[i].cpu_us;
        out->tasks[i].stack_free_min = view->tasks[i].task.stack_hwm;
    }
    out->untracked = view->untracked;
    out->internal.free = s_internal.free_bytes;
    out->internal.min_free = s_internal.min_free_bytes;
    out->internal.largest_free_block = s_internal.largest_free_block;
    out->psram.free = s_psram.free_bytes;
    out->psram.min_free = s_psram.min_free_bytes;
    out->psram.largest_free_block = s_psram.largest_free_block;
    out->chip_temperature_valid = s_chip_temperature_valid;
    out->chip_temperature_c = s_chip_temperature_c;
    xSemaphoreGive(s_mutex);

    diag_lock_view_t lock;

    diag_ot_lock_view(&lock);
    out->lock.acquisitions = lock.since_boot.acquisitions;
    out->lock.hold_us_total = lock.hold_us_total;
    out->lock.wait_us_total = lock.wait_us_total;
    out->lock.max_hold_us = lock.since_boot.max_hold_us;
    out->lock.max_wait_us = lock.since_boot.max_wait_us;
    out->lock.max_hold_60s_us = lock.last_60s.max_hold_us;
    out->lock.max_wait_60s_us = lock.last_60s.max_wait_us;
    out->uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);
    out->reset_reason = reset_reason_name(esp_reset_reason());
    return ESP_OK;
}
