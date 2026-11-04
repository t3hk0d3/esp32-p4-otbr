#pragma once

// Scan state machine (private): discover -> per-router child table and child addresses -> snapshot, over an ops table
// so the host tests can drive it without OpenThread. In parallel from the start, the SRP step browses (and, where a
// browse answer does not describe an instance, resolves) the service types on every other SRP server of the network
// data; finish waits for both, or the cap. ot_ops.c installs the real ops and wraps the core calls in the
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
#define MESH_SCAN_CHILD_DEADLINE_MS 5000 /**< per child address query (ruling R14) */
#define MESH_SCAN_CHILD_QUERIES 4        /**< child address queries in flight at most */

// OpenThread error codes, as plain ints so the host tests need no OpenThread headers (static-asserted in ot_ops.c).
#define MESH_SCAN_OT_ERROR_NONE 0
#define MESH_SCAN_OT_ERROR_NO_BUFS 3
#define MESH_SCAN_OT_ERROR_BUSY 5
#define MESH_SCAN_OT_ERROR_INVALID_STATE 13
#define MESH_SCAN_OT_ERROR_NOT_FOUND 23
#define MESH_SCAN_OT_ERROR_RESPONSE_TIMEOUT 28

typedef struct mesh_scan_ops mesh_scan_ops_t;

/** One service instance from a DNS browse or resolve answer, as ot_ops.c hands it to mesh_scan_on_srp_record(). */
typedef struct {
    char instance[MESH_SCAN_NAME_LEN]; /**< instance label */
    char type[MESH_SCAN_NAME_LEN];     /**< e.g. "_matter._tcp"; the scan files the record under the type it asked */
    char host[MESH_SCAN_NAME_LEN];     /**< host label without the domain; "" when unknown (no SRV) */
    uint16_t port;
    mesh_scan_ip6_t addrs[MESH_SCAN_MAX_SRP_ADDRS];
    uint8_t n_addrs;
    uint32_t ttl_s;
    const uint8_t *txt;   /**< raw TXT RDATA owned by the caller, txt_len bytes (ot_ops gives at most 512) */
    uint16_t txt_len;
    bool has_txt;         /**< a TXT record was found (possibly empty) */
    bool txt_truncated;   /**< ot_ops had to cut the TXT */
    bool described;       /**< SRV, TXT and at least one address found: no resolve needed */
} mesh_scan_srp_record_t;

/** What the state machine needs from OpenThread and the platform. Called with the scan mutex held. */
struct mesh_scan_ops {
    int (*discover)(void);                         /**< start discover; OpenThread error code */
    int (*query_child_table)(uint16_t rloc16);     /**< start a child-table query; OpenThread error code */
    int (*query_child_addrs)(uint16_t rloc16);     /**< start a children-addresses query; OpenThread error code */
    /**
     * Start a Network Diagnostic Get of the IPv6 Address List TLV to the child's RLOC address; its answer reaches
     * mesh_scan_on_child_diag() with `gen`. OpenThread error code.
     */
    int (*query_child_diag)(uint16_t rloc16, uint32_t gen);
    /** One-shot: mesh_scan_on_child_deadline(gen) from the OT task after `ms`; re-arming replaces the previous one. */
    void (*arm_child_deadline)(uint32_t ms, uint32_t gen);
    void (*cancel)(void);                          /**< cancel whatever mesh-diag operation runs */
    /** The local router table: fills both arrays, returns the count. */
    uint8_t (*router_table)(uint16_t rloc16_out[MESH_SCAN_MAX_ROUTERS], uint8_t ids_out[MESH_SCAN_MAX_ROUTERS]);
    /** Fills network, OMR and this board's SRP hosts (TXT into the empty pool); remote hosts are merged after it. */
    void (*snapshot)(mesh_scan_result_t *r);
    bool (*br_running)(void);                      /**< the mode is BR_RUNNING */
    int64_t (*now_ms)(void);
    /**
     * One-shot cap for the scan of generation `gen`; on expiry, mesh_scan_on_cap() from the OT task. Also arms the
     * mode watchdog, which calls mesh_scan_on_mode_lost(gen) once the mode is not BR_RUNNING.
     */
    void (*arm_cap)(uint32_t ms, uint32_t gen);
    void (*disarm_cap)(void);                      /**< stops the cap, the watchdog, the child and SRP deadlines */
    void *(*alloc)(size_t size);                   /**< zeroed memory (PSRAM on target), NULL on failure */
    void (*release)(void *p);                      /**< frees what alloc returned; never called with NULL */
    /**
     * The DNS/SRP unicast servers of the network data (this board's entry marked `this_board`), at most
     * MESH_SCAN_MAX_SRP_SERVERS; returns the count. Also fills what the record filter needs: `net`'s mesh-local and
     * OMR prefixes (the snapshot refreshes them at finish), the extended PAN ID and this board's extended address.
     */
    uint8_t (*srp_servers)(mesh_scan_srp_server_t out[MESH_SCAN_MAX_SRP_SERVERS], mesh_scan_network_t *net,
                           uint8_t ext_pan_id[8], uint8_t own_ext[8]);
    /**
     * Start a DNS-SD browse of `type` (e.g. "_matter._tcp") on `srv`: each instance reaches mesh_scan_on_srp_record()
     * and the end mesh_scan_on_srp_query_done(), both with `gen` and `job`. OpenThread error code; NONE: a done follows.
     */
    int (*dns_browse)(const mesh_scan_srp_server_t *srv, const char *type, uint32_t gen, uint16_t job);
    /** Start a service resolve of `instance`.`type` on `srv`; answers as dns_browse. OpenThread error code. */
    int (*dns_resolve)(const mesh_scan_srp_server_t *srv, const char *instance, const char *type, uint32_t gen,
                       uint16_t job);
    /** One-shot: mesh_scan_on_srp_deadline(gen) from the OT task after `ms`. */
    void (*arm_srp_deadline)(uint32_t ms, uint32_t gen);
};

/** Install the ops (copied). Creates the scan mutex on the first call and returns the scan to idle. */
void mesh_scan_set_ops(const mesh_scan_ops_t *ops);

/**
 * Start a scan. ESP_OK when discover started; ESP_ERR_NOT_ALLOWED when the mode is not BR_RUNNING;
 * ESP_ERR_INVALID_STATE when a scan runs (or the scan mutex was never created); ESP_ERR_NO_MEM (failed, no_memory);
 * ESP_ERR_NOT_FINISHED on OT_ERROR_BUSY (failed, busy); ESP_FAIL on any other discover error (failed, stopped).
 */
esp_err_t mesh_scan_core_start(void);

/** Cancel a running scan (failed, stopped); does nothing otherwise. */
void mesh_scan_core_cancel(void);

/**
 * Fail a running scan with "stopped" without telling OpenThread (scan mutex only, never ops.cancel): for a caller that
 * could not get the OpenThread lock. Does nothing otherwise.
 */
void mesh_scan_core_fail_stopped(void);

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
/**
 * An answer to a child address query of the scan of generation `gen`, from the child with RLOC16 `peer_rloc16`.
 * Dropped unless that child's query is in flight in this scan.
 */
void mesh_scan_on_child_diag(uint32_t gen, uint16_t peer_rloc16, const uint8_t (*addrs)[16], uint8_t n);
/** The child deadline armed for the scan of generation `gen` expired: gives up on the overdue queries. */
void mesh_scan_on_child_deadline(uint32_t gen);
void mesh_scan_on_cap(void);

/**
 * One instance answered to query `job` of the scan of generation `gen`. Dropped unless that query is in flight in the
 * running scan's SRP step. The record is copied; `rec->txt` need not outlive the call.
 */
void mesh_scan_on_srp_record(uint32_t gen, uint16_t job, const mesh_scan_srp_record_t *rec);
/**
 * Query `job` of the scan of generation `gen` ended: NONE or NOT_FOUND (an empty answer) count as an answer from its
 * server, anything else (RESPONSE_TIMEOUT) as none. Dropped unless that query is in flight.
 */
void mesh_scan_on_srp_query_done(uint32_t gen, uint16_t job, int ot_error);
/** The SRP budget of the scan of generation `gen` ran out: the step ends with what it has. */
void mesh_scan_on_srp_deadline(uint32_t gen);

/**
 * The mode left BR_RUNNING during the scan of generation `gen` (the watchdog, esp_timer task). Fails that scan with
 * "stopped" if it still runs. Takes only the scan mutex and never calls ops.cancel: OpenThread may be gone.
 */
void mesh_scan_on_mode_lost(uint32_t gen);

/**
 * The RLOC16 of `peer` when it is an RLOC address in the mesh-local prefix `ml_prefix` (IID 0:ff:fe00:xxxx); false
 * otherwise.
 */
bool mesh_scan_rloc16_from_peer(const uint8_t peer[16], const uint8_t ml_prefix[8], uint16_t *rloc16);

#ifdef __cplusplus
}
#endif
