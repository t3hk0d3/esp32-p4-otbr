#pragma once

// Fake mesh_scan ops for the host tests: records calls, holds scripted return codes, keeps a manual clock and counts
// the heap allocations, one of which can be told to fail.

#include <stdbool.h>
#include <stdint.h>

#include "scan.h"

#define FAKE_MAX_QUERIES 256
#define FAKE_VENDOR_NAME "ESP32-P4-WIFI6-POE-ETH"  /**< this board's info, as the fake snapshot fills it */
#define FAKE_STACK_VERSION "openthread-esp32/test; esp32p4"

/** One dns_browse or dns_resolve call. */
typedef struct {
    bool resolve;
    uint16_t server; /**< RLOC16 of the server asked */
    char type[MESH_SCAN_NAME_LEN];
    char instance[MESH_SCAN_NAME_LEN]; /**< resolve only */
    uint32_t gen;
    uint16_t job;
} fake_dns_call_t;

typedef struct {
    // Scripted inputs.
    bool br_running;
    int discover_ret;
    int child_table_ret;
    int child_addrs_ret;
    int info_rets[FAKE_MAX_QUERIES]; /**< return code of each query_info call, in call order */
    uint8_t table_n; /**< the local router table */
    uint16_t table_rloc16[MESH_SCAN_MAX_ROUTERS];
    uint8_t table_ids[MESH_SCAN_MAX_ROUTERS];
    int64_t now_ms;
    int fail_alloc_at; /**< allocation number (1-based, counted from the reset) that returns NULL, 0 = never */
    uint8_t srp_n;     /**< the network data's SRP servers */
    mesh_scan_srp_server_t srp[MESH_SCAN_MAX_SRP_SERVERS];
    uint8_t ext_pan_id[8];
    uint8_t own_ext[8];
    int dns_rets[FAKE_MAX_QUERIES]; /**< return code of each dns_browse/dns_resolve call, in call order */
    uint8_t snapshot_hosts;         /**< local SRP hosts in the snapshot: "lamp" then "local-<i>", 0 means 1 */
    uint16_t snapshot_pool_used;    /**< pool bytes the snapshot uses up */

    // Recorded calls.
    int discover_calls;
    int child_table_calls;
    int child_addrs_calls;
    uint16_t child_table_rloc16[FAKE_MAX_QUERIES]; /**< in call order */
    uint16_t child_addrs_rloc16[FAKE_MAX_QUERIES];
    int info_calls;
    uint16_t info_rloc16[FAKE_MAX_QUERIES];
    bool info_want_addrs[FAKE_MAX_QUERIES];
    uint32_t info_gen[FAKE_MAX_QUERIES];
    int deadline_calls;
    uint32_t deadline_ms;  /**< of the last arm_child_deadline */
    uint32_t deadline_gen;
    int cancel_calls;
    int router_table_calls;
    int snapshot_calls;
    int arm_calls;
    uint32_t armed_ms;
    uint32_t armed_gen;
    int disarm_calls;
    int srp_servers_calls;
    int dns_calls;
    fake_dns_call_t dns[FAKE_MAX_QUERIES];
    int srp_deadline_calls;
    uint32_t srp_deadline_ms;
    uint32_t srp_deadline_gen;
    int allocs;
    int releases;
    int ops_calls; /**< every op except br_running */
} fake_ops_t;

extern fake_ops_t g_fake;

/** Reset the fake (BR_RUNNING, every op succeeding, clock at 1000 ms) and install it, which returns the scan to idle. */
void fake_ops_install(void);
