#include "zigbee_zlog_map.h"

uint8_t zigbee_zlog_from_esp(esp_log_level_t level)
{
    if (level <= ESP_LOG_NONE) {
        return 0;
    }
    if (level >= ESP_LOG_VERBOSE) {
        return 5;
    }
    return (uint8_t)level; // ERROR..DEBUG are 1..4 in both
}

esp_log_level_t zigbee_zlog_to_esp(uint8_t level)
{
    switch (level) {
    case 0:
    case 1:
        return ESP_LOG_ERROR;
    case 2:
        return ESP_LOG_WARN;
    case 3:
        return ESP_LOG_INFO;
    case 4:
        return ESP_LOG_DEBUG;
    default:
        return ESP_LOG_VERBOSE;
    }
}
