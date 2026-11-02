#pragma once

// Scan state machine (private): discover -> per-router child table and child addresses -> snapshot, over an ops table
// so the host tests can drive it without OpenThread. ot_ops.c installs the real ops and wraps the core calls in the
// OpenThread lock. Lock order everywhere: OpenThread lock, then the scan mutex; the ops never take the OpenThread lock.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "scan_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MESH_SCAN_CAP_MS 60000

// OpenThread error codes, as plain ints so the host tests need no OpenThread headers (static-asserted in ot_ops.c).
#define MESH_SCAN_OT_ERROR_NONE 0
#define MESH_SCAN_OT_ERROR_NO_BUFS 3
#define MESH_SCAN_OT_ERROR_BUSY 5
#define MESH_SCAN_OT_ERROR_INVALID_STATE 13
#define MESH_SCAN_OT_ERROR_RESPONSE_TIMEOUT 28

typedef struct mesh_scan_ops mesh_scan_ops_t;

/** What the state machine needs from OpenThread and the platform. Called with the scan mutex held. */
struct mesh_scan_ops {
    int (*discover)(void);                         /**< start discover; OpenThread error code */
    int (*query_child_table)(uint16_t rloc16);     /**< start a child-table query; OpenThread error code */
    int (*query_child_addrs)(uint16_t rloc16);     /**< start a children-addresses query; OpenThread error code */
    void (*cancel)(void);                          /**< cancel whatever mesh-diag operation runs */
    /** The local router table: fills both arrays, returns the count. */
    uint8_t (*router_table)(uint16_t rloc16_out[MESH_SCAN_MAX_ROUTERS], uint8_t ids_out[MESH_SCAN_MAX_ROUTERS]);
    void (*snapshot)(mesh_scan_result_t *r);       /**< fills network, OMR and SRP */
    bool (*br_running)(void);                      /**< the mode is BR_RUNNING */
    int64_t (*now_ms)(void);
    /**
     * One-shot cap for the scan of generation `gen`; on expiry, mesh_scan_on_cap() from the OT task. Also arms the
     * mode watchdog, which calls mesh_scan_on_mode_lost(gen) once the mode is not BR_RUNNING.
     */
    void (*arm_cap)(uint32_t ms, uint32_t gen);
    void (*disarm_cap)(void);                      /**< stops the cap and the watchdog */
    void *(*alloc)(size_t size);                   /**< zeroed memory (PSRAM on target), NULL on failure */
    void (*release)(void *p);                      /**< frees what alloc returned; never called with NULL */
};

/** Install the ops (copied). Creates the scan mutex on the first call and returns the scan to idle. */
void mesh_scan_set_ops(const mesh_scan_ops_t *ops);

/**
 * Start a scan. ESP_OK when discover started; ESP_ERR_INVALID_STATE when the mode is not BR_RUNNING or a scan runs;
 * ESP_ERR_NO_MEM (failed, no_memory); ESP_ERR_NOT_FINISHED on OT_ERROR_BUSY (failed, busy); ESP_FAIL on any other
 * discover error (failed, stopped).
 */
esp_err_t mesh_scan_core_start(void);

/** Cancel a running scan (failed, stopped); does nothing otherwise. */
void mesh_scan_core_cancel(void);

/**
 * A copy of the state made under the mutex. `result` points at the current result (done only) and holds no
 * reference: it is valid until the next mesh_scan_core_start(). mesh_scan_render() takes a reference instead.
 */
void mesh_scan_view(mesh_scan_view_t *out);

// Callback entry points, called by ot_ops.c from the OpenThread task.

/** One router from discover; `r->children` points at `r->n_children` entries owned by the caller. */
void mesh_scan_on_router(const mesh_scan_router_t *r);
void mesh_scan_on_discover_done(int ot_error);
void mesh_scan_on_child_entry(const mesh_scan_child_t *c);
void mesh_scan_on_child_table_done(int ot_error);
void mesh_scan_on_child_addrs(uint16_t child_rloc16, const uint8_t (*addrs)[16], uint8_t n);
void mesh_scan_on_child_addrs_done(int ot_error);
void mesh_scan_on_cap(void);

/**
 * The mode left BR_RUNNING during the scan of generation `gen` (the watchdog, esp_timer task). Fails that scan with
 * "stopped" if it still runs. Takes only the scan mutex and never calls ops.cancel: OpenThread may be gone.
 */
void mesh_scan_on_mode_lost(uint32_t gen);

#ifdef __cplusplus
}
#endif
