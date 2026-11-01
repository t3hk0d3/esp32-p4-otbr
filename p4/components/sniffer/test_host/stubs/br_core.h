// Host stand-in for the br_core functions the sniffer uses; the tests provide the implementations.
#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "openthread/link_raw.h"

esp_err_t br_core_enter_sniffer(uint8_t channel, otLinkRawReceiveDone receive_cb);
esp_err_t br_core_leave_sniffer(void);
void br_core_set_sniffer_end_cb(void (*cb)(void));

typedef enum {
    BR_MODE_STARTING,
    BR_MODE_BR_RUNNING,
    BR_MODE_SNIFFER,
    BR_MODE_RADIO_FAULT,
} br_core_mode_t;

br_core_mode_t br_core_get_mode(void);
