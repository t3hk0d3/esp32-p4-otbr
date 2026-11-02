// Test-only hooks of br_core (compiled only when OTBR_TEST_BUILD is defined).
#ifdef OTBR_TEST_BUILD

#include "br_core.h"

#include <stdio.h>

#include "esp_check.h"
#include "esp_console.h"
#include "esp_log.h"
#include "esp_openthread_cli.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

static const char *TAG = "br_core_test";

#define NAMESPACE "otbr_dev"
#define KEY_C6_HOLD "test_c6_hold"
#define KEY_PROBE "test_probe"

bool br_core_test_c6_hold(void)
{
    nvs_handle_t handle;
    uint8_t value = 0;

    if (nvs_open(NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }
    nvs_get_u8(handle, KEY_C6_HOLD, &value);
    nvs_close(handle);
    return value != 0;
}

esp_err_t br_core_test_set_c6_hold(bool hold)
{
    nvs_handle_t handle;

    ESP_RETURN_ON_ERROR(nvs_open(NAMESPACE, NVS_READWRITE, &handle), TAG, "nvs_open");
    esp_err_t err = nvs_set_u8(handle, KEY_C6_HOLD, hold ? 1 : 0);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

br_core_test_probe_t br_core_test_probe_override(void)
{
    nvs_handle_t handle;
    uint8_t value = BR_CORE_TEST_PROBE_REAL;

    if (nvs_open(NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return BR_CORE_TEST_PROBE_REAL;
    }
    nvs_get_u8(handle, KEY_PROBE, &value);
    nvs_close(handle);
    return value <= BR_CORE_TEST_PROBE_FAIL ? (br_core_test_probe_t)value : BR_CORE_TEST_PROBE_REAL;
}

esp_err_t br_core_test_set_probe_override(br_core_test_probe_t mode)
{
    nvs_handle_t handle;

    ESP_RETURN_ON_ERROR(nvs_open(NAMESPACE, NVS_READWRITE, &handle), TAG, "nvs_open");
    esp_err_t err = nvs_set_u8(handle, KEY_PROBE, (uint8_t)mode);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

// Console `ot`: the console task (registration) and br_core (OpenThread start and stop) may come in either order, so both
// go through one mutex and the registered command always follows the latest state. Registering an existing name replaces
// the command.
static bool s_console_ready; // br_core_test_ot_command_register was called
static bool s_cli_running;   // OpenThread's CLI is initialised and its task queue exists

static SemaphoreHandle_t ot_command_lock(void)
{
    static StaticSemaphore_t storage;
    static SemaphoreHandle_t mutex;
    static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

    taskENTER_CRITICAL(&mux);
    if (!mutex) {
        mutex = xSemaphoreCreateMutexStatic(&storage);
    }
    taskEXIT_CRITICAL(&mux);
    return mutex;
}

static int ot_command_unavailable(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    printf("Error 13: InvalidState (OpenThread not running)\n");
    return 0;
}

// Called with the lock held.
static esp_err_t ot_command_apply(void)
{
    if (!s_console_ready) {
        return ESP_OK;
    }
    if (s_cli_running) {
        return esp_openthread_cli_console_command_register();
    }
    const esp_console_cmd_t command = {
        .command = "ot",
        .help = "OpenThread CLI (not running now: answers Error 13)",
        .func = ot_command_unavailable,
    };
    return esp_console_cmd_register(&command);
}

esp_err_t br_core_test_ot_command_register(void)
{
    SemaphoreHandle_t lock = ot_command_lock();

    xSemaphoreTake(lock, portMAX_DELAY);
    s_console_ready = true;
    esp_err_t err = ot_command_apply();
    xSemaphoreGive(lock);
    return err;
}

void br_core_test_ot_command_set_running(bool running)
{
    SemaphoreHandle_t lock = ot_command_lock();

    xSemaphoreTake(lock, portMAX_DELAY);
    s_cli_running = running;
    esp_err_t err = ot_command_apply();
    xSemaphoreGive(lock);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "console ot command: %s", esp_err_to_name(err));
    }
}

#endif // OTBR_TEST_BUILD
