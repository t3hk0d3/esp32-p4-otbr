#pragma once

/* Patch 2: mode guard of the Thread REST API and the Web GUI handlers. Pure decision logic (host tested); the
 * callers take the OpenThread lock and ask again while holding it, so a mode change cannot interleave. */

#include <stdbool.h>
#include <stddef.h>
#include "br_core_mode.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ESP_BR_GUARD_NONE,      /**< no OpenThread access (discovery document, static files) */
    ESP_BR_GUARD_READ,      /**< reads OpenThread state: needs a running OpenThread, any mode */
    ESP_BR_GUARD_THREAD_OP, /**< changes Thread/dataset state or needs the radio: border router mode only */
} esp_br_guard_class_t;

/** Class of a Thread REST request by method: GET and HEAD read, everything else changes state. */
#define ESP_BR_GUARD_REST_CLASS(method) \
    (((method) == HTTP_GET || (method) == HTTP_HEAD) ? ESP_BR_GUARD_READ : ESP_BR_GUARD_THREAD_OP)

/**
 * HTTP status for a request of class `cls` in `mode`; `ot_ready` is esp_br_web_openthread_ready().
 * 0 = allowed, 409 = sniffer active (THREAD_OP only), 503 = radio not available (OpenThread not running, or
 * THREAD_OP outside BR_RUNNING). THREAD_OP in SNIFFER is 409 whatever `ot_ready` says. Patch 26: READ and THREAD_OP
 * in NO_RADIO are 503 whatever `ot_ready` says.
 */
int esp_br_guard_status(esp_br_guard_class_t cls, br_core_mode_t mode, bool ot_ready);

/**
 * Human readable reason for a refusal status (409, 503) in `mode`: "sniffer active", "radio not available", and
 * "no radio in OTBR mode" for 503 in NO_RADIO (patch 26); NULL for 0 or any other value.
 */
const char *esp_br_guard_detail(int status, br_core_mode_t mode);

#ifdef __cplusplus
}
#endif
