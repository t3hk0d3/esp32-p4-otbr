#pragma once

// Internal glue between the collector and the HTTP handler.

#include "esp_err.h"
#include "metrics_types.h"

#define METRICS_COLLECT_PERIOD_MS 10000
#define METRICS_COLLECT_LOCK_MS 10

/**
 * Copy the collector's state into `out`. A collector that never started gives started = false and ESP_OK.
 * ESP_ERR_TIMEOUT when the cache mutex is not free within 100 ms.
 */
esp_err_t metrics_collector_copy(metrics_collector_view_t *out);
