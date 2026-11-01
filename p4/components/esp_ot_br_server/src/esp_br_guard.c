#include "esp_br_guard.h"

int esp_br_guard_status(esp_br_guard_class_t cls, br_core_mode_t mode, bool ot_ready)
{
    switch (cls) {
    case ESP_BR_GUARD_NONE:
        return 0;
    case ESP_BR_GUARD_READ:
        return ot_ready ? 0 : 503;
    case ESP_BR_GUARD_THREAD_OP:
        if (mode == BR_MODE_SNIFFER) {
            return 409;
        }
        return (mode == BR_MODE_BR_RUNNING && ot_ready) ? 0 : 503;
    }
    return 503; /* unknown class: refuse */
}

const char *esp_br_guard_detail(int status)
{
    switch (status) {
    case 409:
        return "sniffer active";
    case 503:
        return "radio not available";
    default:
        return NULL;
    }
}
