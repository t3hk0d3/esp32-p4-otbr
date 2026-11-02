#pragma once

// Mesh scan (spec docs/superpowers/specs/2026-11-02-topology-page-design.md): collects the Thread mesh with OpenThread
// mesh diagnostics and renders it as the JSON state object of section 3.3.

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MESH_SCAN_IDLE,
    MESH_SCAN_RUNNING,
    MESH_SCAN_DONE,
    MESH_SCAN_FAILED,
} mesh_scan_state_t;

typedef enum {
    MESH_SCAN_ERR_NONE,
    MESH_SCAN_ERR_BUSY,
    MESH_SCAN_ERR_STOPPED,
    MESH_SCAN_ERR_NO_MEMORY,
} mesh_scan_error_t;

/** Receives each chunk of the document, in order; a non-ESP_OK result stops the render. */
typedef esp_err_t (*mesh_scan_sink_t)(void *ctx, const char *data, size_t len);

esp_err_t mesh_scan_init(void);

/** Start a scan. ESP_ERR_INVALID_STATE while one runs. */
esp_err_t mesh_scan_start(void);

/** Cancel a running scan; it ends failed with error "stopped". */
void mesh_scan_cancel(void);

/** Write the state object through `sink` in chunks of at most 2 KB. Returns the first sink error, or ESP_OK. */
esp_err_t mesh_scan_render(mesh_scan_sink_t sink, void *ctx);

#ifdef __cplusplus
}
#endif
