#pragma once

// GET /metrics (spec docs/superpowers/specs/2026-11-02-prometheus-metrics-design.md): device health and cached Thread
// figures in the Prometheus text format 0.0.4.

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Start the Thread collector (core 1, lowest application priority, every 10 s). ESP_ERR_INVALID_STATE if started
 * already, ESP_ERR_NO_MEM when the task or mutex cannot be created (everything created is released, a later call may
 * retry). Not essential: without it /metrics serves device metrics only.
 */
esp_err_t metrics_start(void);

/** GET /metrics. Never takes the OpenThread lock; waits at most 100 ms for each source. */
esp_err_t metrics_get_handler(httpd_req_t *req);

#ifdef __cplusplus
}
#endif
