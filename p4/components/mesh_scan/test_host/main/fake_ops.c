#include "fake_ops.h"

#include <stdio.h>
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

/** The mesh-local prefix fd.. and the OMR prefix fe.., as ot_ops.c reads them from the network data. */
static void fake_prefixes(mesh_scan_network_t *net)
{
    memset(net->ml_prefix, 0xfd, sizeof(net->ml_prefix));
    memset(net->omr[0], 0xfe, sizeof(net->omr[0]));
    net->n_omr = 1;
}

/**
 * Fills what ot_ops.c's snapshot fills: the network fields, the SLAAC on-mesh prefixes and the SRP hosts ("lamp" with
 * one _matter._tcp service "lamp-svc", then "local-<i>" without services), all local to this board (RLOC16 0x8800).
 */
static void fake_snapshot(mesh_scan_result_t *r)
{
    const uint8_t n = g_fake.snapshot_hosts ? g_fake.snapshot_hosts : 1;

    g_fake.ops_calls++;
    g_fake.snapshot_calls++;
    strcpy(r->network.name, "OpenThread-test");
    r->network.channel = 15;
    r->network.partition_id = 0x12345678;
    r->network.leader_router_id = 9;
    r->network.this_rloc16 = 0x8800;
    fake_prefixes(&r->network);
    for (uint8_t i = 0; i < n; i++) {
        mesh_scan_srp_host_t *h = &r->srp[i];
        snprintf(h->name, sizeof(h->name), i ? "local-%u" : "lamp", (unsigned)i);
        h->n_addrs = 1;
        h->addrs[0][0] = 0xfd;
        h->source = 0x8800;
        h->local = true;
        h->lease_s = 3600;
        h->key_lease_s = 86400;
    }
    strcpy(r->srp[0].services[0].instance, "lamp-svc");
    strcpy(r->srp[0].services[0].type, "_matter._tcp");
    r->srp[0].services[0].port = 5540;
    r->srp[0].services[0].source = 0x8800;
    r->srp[0].n_services = 1;
    r->n_srp = n;
    r->pool_used = g_fake.snapshot_pool_used;
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

static uint8_t fake_srp_servers(mesh_scan_srp_server_t out[MESH_SCAN_MAX_SRP_SERVERS], mesh_scan_network_t *net,
                                uint8_t ext_pan_id[8], uint8_t own_ext[8])
{
    g_fake.ops_calls++;
    g_fake.srp_servers_calls++;
    memcpy(out, g_fake.srp, g_fake.srp_n * sizeof(out[0]));
    fake_prefixes(net);
    memcpy(ext_pan_id, g_fake.ext_pan_id, 8);
    memcpy(own_ext, g_fake.own_ext, 8);
    return g_fake.srp_n;
}

static int fake_dns_call(bool resolve, const mesh_scan_srp_server_t *srv, const char *instance, const char *type,
                         uint32_t gen, uint16_t job)
{
    const int i = g_fake.dns_calls++;

    g_fake.ops_calls++;
    if (i >= FAKE_MAX_QUERIES) {
        return MESH_SCAN_OT_ERROR_NONE;
    }
    fake_dns_call_t *c = &g_fake.dns[i];
    c->resolve = resolve;
    c->server = srv->rloc16;
    snprintf(c->type, sizeof(c->type), "%s", type);
    snprintf(c->instance, sizeof(c->instance), "%s", instance ? instance : "");
    c->gen = gen;
    c->job = job;
    return g_fake.dns_rets[i];
}

static int fake_dns_browse(const mesh_scan_srp_server_t *srv, const char *type, uint32_t gen, uint16_t job)
{
    return fake_dns_call(false, srv, NULL, type, gen, job);
}

static int fake_dns_resolve(const mesh_scan_srp_server_t *srv, const char *instance, const char *type, uint32_t gen,
                            uint16_t job)
{
    return fake_dns_call(true, srv, instance, type, gen, job);
}

static void fake_arm_srp_deadline(uint32_t ms, uint32_t gen)
{
    g_fake.ops_calls++;
    g_fake.srp_deadline_calls++;
    g_fake.srp_deadline_ms = ms;
    g_fake.srp_deadline_gen = gen;
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
    .srp_servers = fake_srp_servers,
    .dns_browse = fake_dns_browse,
    .dns_resolve = fake_dns_resolve,
    .arm_srp_deadline = fake_arm_srp_deadline,
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
