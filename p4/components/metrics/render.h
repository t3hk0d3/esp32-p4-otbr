#pragma once

// GET /metrics body (spec section 4) in the Prometheus text format 0.0.4. Pure: host-tested in test_host/.

#include "esp_err.h"
#include "metrics_types.h"
#include "writer.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Write every family that has samples, in the catalogue's order, through `w`. Returns the writer's first flush error,
 * or ESP_OK. Call metrics_writer_finish afterwards to send the rest.
 */
esp_err_t metrics_render(metrics_writer_t *w, const metrics_input_t *in);

#ifdef __cplusplus
}
#endif
