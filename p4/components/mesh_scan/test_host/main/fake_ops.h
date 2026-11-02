#pragma once

// Fake mesh_scan ops for the host tests: records calls, holds scripted return codes, keeps a manual clock and counts
// the heap allocations, one of which can be told to fail.

#include <stdbool.h>
#include <stdint.h>

#include "scan.h"

#define FAKE_MAX_QUERIES 64

typedef struct {
    // Scripted inputs.
    bool br_running;
    int discover_ret;
    int child_table_ret;
    int child_addrs_ret;
    uint8_t table_n; /**< the local router table */
    uint16_t table_rloc16[MESH_SCAN_MAX_ROUTERS];
    uint8_t table_ids[MESH_SCAN_MAX_ROUTERS];
    int64_t now_ms;
    int fail_alloc_at; /**< allocation number (1-based, counted from the reset) that returns NULL, 0 = never */

    // Recorded calls.
    int discover_calls;
    int child_table_calls;
    int child_addrs_calls;
    uint16_t child_table_rloc16[FAKE_MAX_QUERIES]; /**< in call order */
    uint16_t child_addrs_rloc16[FAKE_MAX_QUERIES];
    int cancel_calls;
    int router_table_calls;
    int snapshot_calls;
    int arm_calls;
    uint32_t armed_ms;
    uint32_t armed_gen;
    int disarm_calls;
    int allocs;
    int releases;
    int ops_calls; /**< every op except br_running */
} fake_ops_t;

extern fake_ops_t g_fake;

/** Reset the fake (BR_RUNNING, every op succeeding, clock at 1000 ms) and install it, which returns the scan to idle. */
void fake_ops_install(void);
