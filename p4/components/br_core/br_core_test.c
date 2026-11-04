// Test-only hooks of br_core (compiled only when OTBR_TEST_BUILD is defined).
#ifdef OTBR_TEST_BUILD

#include "br_core.h"

#include "esp_check.h"
#include "esp_log.h"
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

#endif // OTBR_TEST_BUILD
