#pragma once

// GET /device/zigbee body (zigbee_json.c). Pure: host-tested in test_host/.

#include <stddef.h>
#include "esp_err.h"
#include "zigbee_status.h"

#ifdef __cplusplus
extern "C" {
#endif

/** A buffer that holds the body for any status: the longest strings, every escape, every number at its widest. */
#define ZIGBEE_JSON_MAX 3072

/**
 * The /device/zigbee body: {channel, firmware, state, reason, client, last_client, radio, counters, heap, started}.
 * `state` is idle|starting|running|no_radio|incompatible. client {address, since} and last_client {address, left} are
 * null when absent; times are epoch seconds (0: the board clock was not set). radio {channel, pan_id, short_address,
 * extended_address (16 lowercase hex digits, in the status's byte order), tx_power_dbm} is null without a radio
 * configuration. counters holds the status counters by name, heap {current, peak}. started is null while the stack has
 * not started, else the epoch seconds (0 with the clock not set).
 * ESP_ERR_INVALID_SIZE when it does not fit `cap`; `buf` then holds a truncated, NUL-terminated prefix.
 * `*len` is the length written.
 */
esp_err_t zigbee_json_status(const zigbee_status_t *st, char *buf, size_t cap, size_t *len);

#ifdef __cplusplus
}
#endif
