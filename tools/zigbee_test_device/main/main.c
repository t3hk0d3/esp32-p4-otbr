#include "esp_check.h"
#include "nvs_flash.h"

#include "light.h"
#include "repl.h"
#include "zigbee_node.h"

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(light_init());
    ESP_ERROR_CHECK(repl_start());
    ESP_ERROR_CHECK(zigbee_node_start());
}
