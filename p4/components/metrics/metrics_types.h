#pragma once

// Inputs of the renderer: plain copies of each source, filled by device_api, scrape.c and the collector. No OpenThread or
// FreeRTOS types, so that render.c is host-tested.

#include <stdbool.h>
#include <stdint.h>

#include "diag.h"
#include "metrics.h"

#ifdef __cplusplus
extern "C" {
#endif

#define METRICS_ROLE_COUNT 5 /**< otDeviceRole: disabled, detached, child, router, leader */

/** Figures the collector reads under the OpenThread lock. */
typedef struct {
    int role; /**< otDeviceRole */
    uint32_t role_entered[METRICS_ROLE_COUNT];
    uint32_t partition_changes;
    uint32_t attach_attempts;
    uint32_t routers;
    uint32_t children;
    uint32_t neighbors;
    struct {
        uint32_t tx_total, tx_retry, tx_err_cca, tx_err_abort, tx_err_busy_channel;
        uint32_t rx_total, rx_duplicated, rx_err_fcs, rx_err_sec, rx_err_no_frame, rx_err_unknown_neighbor,
                 rx_err_invalid_src, rx_err_other;
    } mac;
    struct {
        uint32_t tx_success, tx_failure, rx_success, rx_failure;
    } ip6;
    int br_state; /**< otBorderRoutingState: uninitialized, disabled, stopped, running */
    int pd_state; /**< otBorderRoutingDhcp6PdState: disabled, stopped, running, idle; -1 unknown */
    uint64_t br_packets[2][2]; /**< [inbound, outbound][unicast, multicast] */
    uint64_t br_bytes[2][2];
    uint32_t ra[3]; /**< rx, tx_success, tx_failure */
    uint32_t rs[3];
    int srp_state; /**< otSrpServerState: disabled, running, stopped */
    uint32_t srp_hosts;
    uint32_t srp_services;
    struct {
        bool built;   /**< false: no TREL families */
        bool enabled; /**< the setting */
        bool active;  /**< OpenThread runs TREL */
        uint32_t peers;
        uint64_t tx_packets, tx_bytes, tx_failures, rx_packets, rx_bytes;
    } trel;
} metrics_thread_t;

typedef struct {
    bool started;        /**< the collector task runs */
    bool ever_collected; /**< at least one collection succeeded: age_ms is meaningful */
    bool valid;          /**< the cache holds figures of the current BR_RUNNING period */
    uint32_t age_ms;     /**< since the last successful collection */
    uint32_t failures;   /**< collections skipped because the lock was not free */
    metrics_thread_t thread;
} metrics_collector_view_t;

typedef struct {
    const metrics_device_t *device;
    const diag_metrics_view_t *diag;              /**< NULL: diag not started or busy */
    const metrics_collector_view_t *collector;    /**< NULL: the cache could not be copied */
} metrics_input_t;

#define METRICS_FRESH_MS 30000 /**< Thread series are left out once the cache is this old */

#ifdef __cplusplus
}
#endif
