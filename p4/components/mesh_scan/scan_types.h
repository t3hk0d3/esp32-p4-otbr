#pragma once

// Result of one mesh scan. Pure data: filled by the collector, read by the renderer.

#include <stdbool.h>
#include <stdint.h>

#include "mesh_scan.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MESH_SCAN_MAX_ROUTERS 63 /**< indexed by router ID */
#define MESH_SCAN_MAX_CHILDREN 128
#define MESH_SCAN_MAX_ROUTER_ADDRS 16
#define MESH_SCAN_MAX_CHILD_ADDRS 8
#define MESH_SCAN_MAX_SRP_HOSTS 64
#define MESH_SCAN_MAX_SRP_ADDRS 8
#define MESH_SCAN_MAX_SRP_SERVICES 8
#define MESH_SCAN_NAME_LEN 64
#define MESH_SCAN_MAX_OMR 4

/** An IPv6 address or prefix, network byte order. */
typedef uint8_t mesh_scan_ip6_t[16];
/** The first 64 bits of an address. */
typedef uint8_t mesh_scan_prefix_t[8];

typedef struct {
    uint16_t rloc16;
    uint8_t ext[8];
    bool has_ext;
    bool rx_on_when_idle;
    bool full_thread_device;
    bool full_network_data;
    uint8_t lq_in;
    uint32_t timeout_s;
    uint32_t age_s;
    uint32_t connected_s;
    int8_t rssi_avg;
    int8_t rssi_last;
    uint8_t link_margin_db;
    bool has_err_rate;  /**< the parent tracks the error rates; frame_err and msg_err are meaningless otherwise */
    uint16_t frame_err; /**< raw 0..0xffff, 0xffff = 100 % */
    uint16_t msg_err;   /**< raw 0..0xffff */
    uint16_t version;
    bool csl;
    mesh_scan_ip6_t addrs[MESH_SCAN_MAX_CHILD_ADDRS];
    uint8_t n_addrs;
    bool addresses_reported; /**< false: asked directly (spec 3.4) and gave no answer; true otherwise */
    bool ask_pending;        /**< internal to scan.c: picked for the child address step, not answered yet */
} mesh_scan_child_t;

typedef struct {
    bool present;
    bool responded; /**< false: stub from the local router table, only router_id and rloc16 are valid */
    uint16_t rloc16;
    uint8_t router_id;
    uint8_t ext[8];
    bool has_version; /**< the router reported its Thread version */
    uint16_t version;
    bool leader;
    bool this_device;
    bool border_router;
    uint8_t lq_in[MESH_SCAN_MAX_ROUTERS]; /**< incoming LQ from each router ID, 0 = no link */
    mesh_scan_ip6_t addrs[MESH_SCAN_MAX_ROUTER_ADDRS];
    uint8_t n_addrs;
    bool children_detail;
    mesh_scan_child_t *children; /**< n_children entries allocated by scan.c through ops.alloc; NULL when 0 */
    uint16_t n_children;
} mesh_scan_router_t;

typedef struct {
    char name[MESH_SCAN_NAME_LEN];
    mesh_scan_ip6_t addrs[MESH_SCAN_MAX_SRP_ADDRS];
    uint8_t n_addrs;
    struct {
        char instance[MESH_SCAN_NAME_LEN];
        char type[MESH_SCAN_NAME_LEN];
    } services[MESH_SCAN_MAX_SRP_SERVICES];
    uint8_t n_services;
} mesh_scan_srp_host_t;

typedef struct {
    char name[MESH_SCAN_NAME_LEN];
    uint8_t channel;
    uint32_t partition_id;
    uint8_t leader_router_id;
    mesh_scan_prefix_t ml_prefix;
    uint16_t this_rloc16;
    mesh_scan_prefix_t omr[MESH_SCAN_MAX_OMR];
    uint8_t n_omr;
} mesh_scan_network_t;

/**
 * About 100 KB (the SRP host array alone is 64 x ~1.2 KB): allocated by scan.c through `ops.alloc` (PSRAM on target),
 * never on a stack. Up to two can exist at once: a new scan allocates its result while a render still holds the old one.
 */
typedef struct {
    mesh_scan_network_t network;
    mesh_scan_router_t routers[MESH_SCAN_MAX_ROUTERS]; /**< by router ID; absent entries have present == false */
    mesh_scan_srp_host_t srp[MESH_SCAN_MAX_SRP_HOSTS];
    uint8_t n_srp;
    bool partial;
} mesh_scan_result_t;

/** What the renderer needs: a consistent copy of the state, valid for the duration of one render. */
typedef struct {
    mesh_scan_state_t state;
    mesh_scan_error_t error; /**< failed only */
    uint8_t routers;         /**< running: routers discovered, 0 until discover ends */
    uint8_t queried;         /**< running: routers finished */
    uint16_t children_total;   /**< running: children asked for their addresses, 0 before that step */
    uint16_t children_queried; /**< running: of those, answered or given up */
    uint32_t age_s;          /**< done and failed: seconds since the scan ended */
    const mesh_scan_result_t *result; /**< done only, non-NULL then */
} mesh_scan_view_t;

#ifdef __cplusplus
}
#endif
