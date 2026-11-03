#include "supervisor.h"

#include <stdatomic.h>
#include <stdbool.h>

#include "board_cores.h"
#include "br_core.h"
#include "esp_br_web.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_openthread_lock.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "supervisor";

#define TASK_STACK 3072
#define TASK_PRIORITY 3
#define OT_LOCK_FAILURES_TO_ABORT 2

static TaskHandle_t s_task;
static atomic_bool s_httpd_pending; // a probe is queued and has not run yet
static int64_t s_httpd_sent_us;

/* HTTP server task: the probe ran, so the server task is not stuck. */
static void httpd_probe(void *arg)
{
    (void)arg;
    atomic_store(&s_httpd_pending, false);
}

static void check_httpd(void)
{
    httpd_handle_t server = esp_br_web_get_server();

    if (!server) {
        atomic_store(&s_httpd_pending, false);
        return;
    }
    const int64_t now = esp_timer_get_time();

    if (atomic_load(&s_httpd_pending)) {
        if (now - s_httpd_sent_us > (int64_t)SUPERVISOR_HTTPD_STALL_S * 1000000) {
            esp_system_abort("supervisor: the HTTP server task is stuck");
        }
        return;
    }
    atomic_store(&s_httpd_pending, true);
    s_httpd_sent_us = now;
    if (httpd_queue_work(server, httpd_probe, NULL) != ESP_OK) {
        atomic_store(&s_httpd_pending, false); // not queued (control socket busy): try again next period
    }
}

/* False only when the lock could not be taken although OpenThread is up. */
static bool check_ot_lock(void)
{
    if (!br_core_ot_session_begin()) {
        return true; // OpenThread is not up or a radio push is stopping it: nothing to probe
    }
    const bool taken = esp_openthread_lock_acquire(pdMS_TO_TICKS(SUPERVISOR_OT_LOCK_WAIT_S * 1000));

    if (taken) {
        esp_openthread_lock_release();
    }
    br_core_ot_session_end();
    return taken;
}

static void supervisor_task(void *arg)
{
    (void)arg;
    int ot_failures = 0;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(SUPERVISOR_PERIOD_S * 1000));
        check_httpd();
        if (check_ot_lock()) {
            ot_failures = 0;
        } else if (++ot_failures >= OT_LOCK_FAILURES_TO_ABORT) {
            esp_system_abort("supervisor: the OpenThread lock is stuck");
        } else {
            ESP_LOGE(TAG, "the OpenThread lock was not free for %d s", SUPERVISOR_OT_LOCK_WAIT_S);
        }
    }
}

esp_err_t supervisor_start(void)
{
    if (s_task) {
        return ESP_ERR_INVALID_STATE;
    }
    return xTaskCreatePinnedToCore(supervisor_task, "supervisor", TASK_STACK, NULL, TASK_PRIORITY, &s_task, MGMT_CORE) ==
                   pdPASS
               ? ESP_OK
               : ESP_ERR_NO_MEM;
}
