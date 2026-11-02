#pragma once

// JSON renderer for the state object (spec section 3.3). Pure: host-tested in test_host/.

#include "scan_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Write `view` as JSON through `sink` in chunks of at most 2 KB. A running view writes progress and never reads
 * `result`; a done view needs a non-NULL `result` (ESP_ERR_INVALID_ARG otherwise). Returns the first sink error.
 */
esp_err_t mesh_scan_render_view(const mesh_scan_view_t *view, mesh_scan_sink_t sink, void *ctx);

#ifdef __cplusplus
}
#endif
