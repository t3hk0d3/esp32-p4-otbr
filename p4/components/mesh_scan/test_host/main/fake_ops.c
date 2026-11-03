#include "fake_ops.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

fake_ops_t g_fake;

static int fake_discover(void)
{
    g_fake.ops_calls++;
    g_fake.discover_calls++;
    return g_fake.discover_ret;
}

static int fake_query_child_table(uint16_t rloc16)
{
    g_fake.ops_calls++;
    if (g_fake.child_table_calls < FAKE_MAX_QUERIES) {
        g_fake.child_table_rloc16[g_fake.child_table_calls] = rloc16;
    }
    g_fake.child_table_calls++;
    return g_fake.child_table_ret;
}

static int fake_query_child_addrs(uint16_t rloc16)
{
    g_fake.ops_calls++;
    if (g_fake.child_addrs_calls < FAKE_MAX_QUERIES) {
        g_fake.child_addrs_rloc16[g_fake.child_addrs_calls] = rloc16;
    }
    g_fake.child_addrs_calls++;
    return g_fake.child_addrs_ret;
}

static int fake_query_child_diag(uint16_t rloc16, uint32_t gen)
{
    const int i = g_fake.child_diag_calls++;

    g_fake.ops_calls++;
    if (i >= FAKE_MAX_QUERIES) {
        return MESH_SCAN_OT_ERROR_NONE;
    }
    g_fake.child_diag_rloc16[i] = rloc16;
    g_fake.child_diag_gen[i] = gen;
    return g_fake.child_diag_rets[i];
}

static void fake_arm_child_deadline(uint32_t ms, uint32_t gen)
{
    g_fake.ops_calls++;
    g_fake.deadline_calls++;
    g_fake.deadline_ms = ms;
    g_fake.deadline_gen = gen;
}

static void fake_cancel(void)
{
    g_fake.ops_calls++;
    g_fake.cancel_calls++;
}

static uint8_t fake_router_table(uint16_t rloc16_out[MESH_SCAN_MAX_ROUTERS], uint8_t ids_out[MESH_SCAN_MAX_ROUTERS])
{
    g_fake.ops_calls++;
    g_fake.router_table_calls++;
    memcpy(rloc16_out, g_fake.table_rloc16, sizeof(g_fake.table_rloc16));
    memcpy(ids_out, g_fake.table_ids, sizeof(g_fake.table_ids));
    return g_fake.table_n;
}

/** Fills what ot_ops.c's snapshot fills: the network fields, the SLAAC on-mesh prefixes and the SRP hosts. */
static void fake_snapshot(mesh_scan_result_t *r)
{
    g_fake.ops_calls++;
    g_fake.snapshot_calls++;
    strcpy(r->network.name, "OpenThread-test");
    r->network.channel = 15;
    r->network.partition_id = 0x12345678;
    r->network.leader_router_id = 9;
    r->network.this_rloc16 = 0x8800;
    memset(r->network.ml_prefix, 0xfd, sizeof(r->network.ml_prefix));
    memset(r->network.omr[0], 0xfe, sizeof(r->network.omr[0]));
    r->network.n_omr = 1;
    strcpy(r->srp[0].name, "lamp");
    r->srp[0].n_addrs = 1;
    r->srp[0].addrs[0][0] = 0xfd;
    r->n_srp = 1;
}

static bool fake_br_running(void)
{
    return g_fake.br_running;
}

static int64_t fake_now_ms(void)
{
    g_fake.ops_calls++;
    return g_fake.now_ms;
}

static void fake_arm_cap(uint32_t ms, uint32_t gen)
{
    g_fake.ops_calls++;
    g_fake.arm_calls++;
    g_fake.armed_ms = ms;
    g_fake.armed_gen = gen;
}

static void fake_disarm_cap(void)
{
    g_fake.ops_calls++;
    g_fake.disarm_calls++;
}

static void *fake_alloc(size_t size)
{
    g_fake.ops_calls++;
    g_fake.allocs++;
    if (g_fake.fail_alloc_at && g_fake.allocs == g_fake.fail_alloc_at) {
        return NULL;
    }
    return calloc(1, size);
}

static void fake_release(void *p)
{
    g_fake.ops_calls++;
    if (p) {
        g_fake.releases++;
    }
    free(p);
}

static const mesh_scan_ops_t FAKE_OPS = {
    .discover = fake_discover,
    .query_child_table = fake_query_child_table,
    .query_child_addrs = fake_query_child_addrs,
    .query_child_diag = fake_query_child_diag,
    .arm_child_deadline = fake_arm_child_deadline,
    .cancel = fake_cancel,
    .router_table = fake_router_table,
    .snapshot = fake_snapshot,
    .br_running = fake_br_running,
    .now_ms = fake_now_ms,
    .arm_cap = fake_arm_cap,
    .disarm_cap = fake_disarm_cap,
    .alloc = fake_alloc,
    .release = fake_release,
};

void fake_ops_install(void)
{
    // The scan logs every failure the tests provoke on purpose.
    esp_log_level_set("mesh_scan", ESP_LOG_ERROR);
    // Installing returns the scan to idle and may release the previous test's result through these ops.
    mesh_scan_set_ops(&FAKE_OPS);
    memset(&g_fake, 0, sizeof(g_fake));
    g_fake.br_running = true;
    g_fake.now_ms = 1000;
}
