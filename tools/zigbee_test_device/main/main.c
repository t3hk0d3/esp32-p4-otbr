#include "esp_check.h"
#include "nvs_flash.h"

#include "light.h"
#include "repl.h"
#include "zigbee_node.h"

void app_main(void)
{
    /* The aux C6 is shared: its NVS may hold another project's data or a newer layout. */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(light_init());
    ESP_ERROR_CHECK(repl_start());
    ESP_ERROR_CHECK(zigbee_node_start());
}
