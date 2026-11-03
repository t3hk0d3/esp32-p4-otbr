#include "reboot_gate.h"

#include <stdatomic.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "reboot_gate";

#define RETRY_MS 2000

typedef bool (*busy_fn_t)(void);

static _Atomic(busy_fn_t) s_busy;
static esp_timer_handle_t s_timer;
static atomic_bool s_logged;

static void on_timer(void *arg);

void reboot_gate_set_busy(bool (*busy)(void))
{
    if (!s_timer) {
        const esp_timer_create_args_t args = {.callback = on_timer, .name = "reboot_gate"};
        if (esp_timer_create(&args, &s_timer) != ESP_OK) {
            s_timer = NULL;
            ESP_LOGE(TAG, "cannot create the reboot timer: reboots wait in their caller");
        }
    }
    atomic_store(&s_busy, busy);
}

bool reboot_gate_busy(void)
{
    const busy_fn_t busy = atomic_load(&s_busy);
    return busy && busy();
}

static void on_timer(void *arg)
{
    (void)arg;
    if (reboot_gate_busy()) {
        if (!atomic_exchange(&s_logged, true)) {
            ESP_LOGW(TAG, "reboot deferred: a long operation is running");
        }
        esp_timer_start_once(s_timer, (uint64_t)RETRY_MS * 1000);
        return;
    }
    ESP_LOGW(TAG, "rebooting");
    esp_restart();
}

void reboot_gate_restart(uint32_t delay_ms)
{
    if (s_timer) {
        esp_err_t err = esp_timer_start_once(s_timer, (uint64_t)delay_ms * 1000);
        if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) { // INVALID_STATE: a reboot is already scheduled
            return;
        }
    }
    // No timer (reboot_gate_set_busy not called or the timer could not be created): wait here.
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
    while (reboot_gate_busy()) {
        vTaskDelay(pdMS_TO_TICKS(RETRY_MS));
    }
    esp_restart();
}
