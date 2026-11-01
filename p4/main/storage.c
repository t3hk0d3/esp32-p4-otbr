#include "storage.h"

#include <stdbool.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_spiffs.h"
#include "nvs_flash.h"

static const char *TAG = "storage";

#define WEB_PATH "/web"
#define MAX_OPEN_FILES 5

// Mount a SPIFFS partition. An image that fails to mount is never formatted; the error goes to the caller.
static esp_err_t mount_spiffs(const char *label, const char *path)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = path,
        .partition_label = label,
        .max_files = MAX_OPEN_FILES,
        .format_if_mount_failed = false,
    };
    ESP_RETURN_ON_ERROR(esp_vfs_spiffs_register(&conf), TAG, "mounting %s at %s failed", label, path);

    size_t total = 0;
    size_t used = 0;
    ESP_RETURN_ON_ERROR(esp_spiffs_info(label, &total, &used), TAG, "reading %s info failed", label);
    ESP_LOGI(TAG, "mounted %s at %s (total %u, used %u)", label, path, (unsigned)total, (unsigned)used);
    return ESP_OK;
}

esp_err_t storage_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS written by a newer version, erasing");
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "erasing NVS failed");
        err = nvs_flash_init();
    }
    return err;
}

int storage_running_slot(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!running) {
        return -1;
    }
    switch (running->subtype) {
    case ESP_PARTITION_SUBTYPE_APP_OTA_0:
        return 0;
    case ESP_PARTITION_SUBTYPE_APP_OTA_1:
        return 1;
    default:
        return -1;
    }
}

esp_err_t storage_mount(void)
{
    const int slot = storage_running_slot();
    ESP_RETURN_ON_FALSE(slot >= 0, ESP_ERR_INVALID_STATE, TAG, "not running from ota_0 or ota_1");
    ESP_LOGI(TAG, "running slot %d", slot);

    return mount_spiffs(slot == 0 ? "web_0" : "web_1", WEB_PATH);
}
