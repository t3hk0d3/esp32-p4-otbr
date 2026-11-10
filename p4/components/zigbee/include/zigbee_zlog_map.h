#pragma once

#include <stdint.h>

#include "esp_log.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Level maps between ESP-IDF's esp_log_level_t and ziggurat's log levels (0 off, 1 error, 2 warning, 3 info, 4 debug,
 * 5 trace). Pure, so the host tests cover them.
 */

/** The ziggurat level for an ESP level: NONE 0, ERROR 1, WARN 2, INFO 3, DEBUG 4, VERBOSE 5; out of range clamps. */
uint8_t zigbee_zlog_from_esp(esp_log_level_t level);

/** The ESP level a ziggurat line of `level` is printed at: 1 ERROR .. 5 VERBOSE; 0 reads as ERROR, above 5 as VERBOSE. */
esp_log_level_t zigbee_zlog_to_esp(uint8_t level);

#ifdef __cplusplus
}
#endif
