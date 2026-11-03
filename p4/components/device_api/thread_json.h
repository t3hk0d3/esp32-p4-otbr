#pragma once

// /device/thread bodies (thread_json.c). Pure: host-tested in test_host/. Includes only the host-safe br_core_leader.h.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "br_core_leader.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool has_adjustment;
    int8_t adjustment;
    bool become_leader;
} device_api_thread_put_t;

/**
 * Parse a PUT /device/thread body: an object with "leader_weight_adjustment" (an integer value -16..16; 8.0 counts as 8)
 * and/or "become_leader" (a boolean; false means nothing to do). Unknown keys are ignored. ESP_ERR_INVALID_ARG with
 * *field = the bad key, or "body" when it is not a JSON object or has neither key.
 */
esp_err_t device_api_thread_parse(const char *body, device_api_thread_put_t *out, const char **field);

typedef struct {
    int8_t adjustment;
    bool weight_known;  /**< write leader_weight and leader_weight_base */
    uint8_t weight;
    bool partial;       /**< the weight could not be read in time */
    bool has_applied;   /**< PUT reply: write "applied" */
    bool applied;
    const char *detail; /**< written after "applied" when not NULL and not empty */
    bool become_started;
} device_api_thread_t;

/** The GET body, or the PUT reply with applied / detail / become_leader. Returns its length, or -1 when `cap` is too small. */
int device_api_thread_json(char *buf, size_t cap, const device_api_thread_t *t);

/**
 * HTTP answer to a refused take-over. `mode` is a br_core_mode_t (BR_CORE_LEADER_NOT_RUNNING reports it as "mode <NAME>").
 * Writes the detail into `detail` (`cap` bytes).
 */
void device_api_thread_refusal(br_core_leader_refusal_t refusal, int mode, int *status, const char **code, char *detail,
                               size_t cap);

#ifdef __cplusplus
}
#endif
