#pragma once

// GET /metrics (spec docs/superpowers/specs/2026-11-02-prometheus-metrics-design.md): device health and cached Thread
// figures in the Prometheus text format 0.0.4. device_api owns the HTTP side and passes the device figures in.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define METRICS_CONTENT_TYPE "text/plain; version=0.0.4; charset=utf-8"

/** Zigbee figures (a copy of zigbee_status_t's), written only while a channel runs ziggurat. */
typedef struct {
    bool active; /**< false: no Zigbee series */
    uint32_t frames_rx, frames_tx;
    uint32_t tx_no_ack, tx_cca, tx_failed; /**< transmit results; frames_tx counts the accepted transmits, so no "acked" */
    uint32_t rcp_resets;
    uint64_t heap_current, heap_peak; /**< bytes */
    bool client_connected;
} metrics_zigbee_t;

/** Figures that need no OpenThread lock, gathered by the caller. */
typedef struct {
    int mode;                /**< br_core_mode_t: STARTING, BR_RUNNING, SNIFFER, RADIO_FAULT */
    const char *version;
    int slot;                /**< running OTA slot, -1 unknown */
    const char *rcp_version; /**< "" unknown or none */
    bool rcp_compatible;
    bool storage_ok;
    bool sniffer_active;
    uint32_t sniffer_frames;
    uint32_t sniffer_drops;
    bool eth_up;
    bool trial_pending;
    metrics_zigbee_t zigbee;
} metrics_device_t;

/** Receives each chunk of the body, in order; a non-ESP_OK result stops the body. */
typedef esp_err_t (*metrics_flush_fn)(void *ctx, const char *data, size_t len);

/**
 * Start the Thread collector (core 1, lowest application priority, every 10 s). ESP_ERR_INVALID_STATE if started
 * already, ESP_ERR_NO_MEM when the task or mutex cannot be created (everything created is released, a later call may
 * retry). Not essential: without it /metrics serves device metrics only.
 */
esp_err_t metrics_start(void);

/**
 * Write the whole body through `flush` in chunks of at most 2 KB. Copies the diag view, the collector cache and the
 * routing view once (never the OpenThread lock). ESP_ERR_NO_MEM before anything was flushed; otherwise the first flush
 * error, or ESP_OK. The caller ends the response.
 */
esp_err_t metrics_write(const metrics_device_t *device, metrics_flush_fn flush, void *ctx);

#ifdef __cplusplus
}
#endif
