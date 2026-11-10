#include "ot_log_map.h"

esp_log_level_t br_core_ot_log_to_esp(otLogLevel level)
{
    switch (level) {
    case OT_LOG_LEVEL_CRIT:
        return ESP_LOG_ERROR;
    case OT_LOG_LEVEL_WARN:
        return ESP_LOG_WARN;
    case OT_LOG_LEVEL_NOTE:
    case OT_LOG_LEVEL_INFO:
        return ESP_LOG_INFO;
    default:
        return ESP_LOG_DEBUG;
    }
}

otLogLevel br_core_esp_to_ot_log(esp_log_level_t level)
{
    switch (level) {
    case ESP_LOG_NONE:
        return OT_LOG_LEVEL_NONE;
    case ESP_LOG_ERROR:
        return OT_LOG_LEVEL_CRIT;
    case ESP_LOG_WARN:
        return OT_LOG_LEVEL_WARN;
    case ESP_LOG_INFO:
        return OT_LOG_LEVEL_INFO;
    default:
        return OT_LOG_LEVEL_DEBG;
    }
}
