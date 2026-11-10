#pragma once

// Level maps between OpenThread's log levels and ESP-IDF's (ot_log.c uses them). Pure, so host-tested.

#include "esp_log_level.h"
#include "openthread/platform/logging.h"

#ifdef __cplusplus
extern "C" {
#endif

/** OpenThread line level to the ESP level it is logged at: CRIT error, WARN warning, NOTE and INFO info, DEBG debug. */
esp_log_level_t br_core_ot_log_to_esp(otLogLevel level);

/** ESP effective level to the OpenThread level that lets that much through: error CRIT, warning WARN, info INFO, debug and verbose DEBG, off NONE. */
otLogLevel br_core_esp_to_ot_log(esp_log_level_t level);

#ifdef __cplusplus
}
#endif
