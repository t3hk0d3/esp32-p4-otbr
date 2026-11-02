#include <stdlib.h>
#include <string.h>

#include "fake_ops.h"
#include "scan.h"
#include "unity.h"

#define RLOC16(id) ((uint16_t)((id) << 10))

static mesh_scan_router_t s_router;
static mesh_scan_child_t s_children[MESH_SCAN_MAX_CHILDREN + 4];

/** A discover report for router `id` with `n` children (RLOC16 = router RLOC16 + 1 + i, sleepy, lq_in 2). */
static const mesh_scan_router_t *router(uint8_t id, uint16_t n)
{
    memset(&s_router, 0, sizeof(s_router));
    memset(s_children, 0, sizeof(s_children));
    s_router.router_id = id;
    s_router.rloc16 = RLOC16(id);
    s_router.version = 5;
    s_router.ext[0] = id;
    for (uint16_t i = 0; i < n; i++) {
        s_children[i].rloc16 = (uint16_t)(RLOC16(id) + 1 + i);
        s_children[i].lq_in = 2;
    }
    s_router.children = n ? s_children : NULL;
    s_router.n_children = n;
    return &s_router;
}

/** A child-table entry for `rloc16`, every table field set. */
static mesh_scan_child_t table_entry(uint16_t rloc16)
{
    return (mesh_scan_child_t) {
        .rloc16 = rloc16, .ext = { 0xaa, 1, 2, 3, 4, 5, 6, (uint8_t)rloc16 }, .rx_on_when_idle = true,
        .full_thread_device = true, .full_network_data = true, .lq_in = 3, .timeout_s = 240, .age_s = 12,
        .connected_s = 86400, .rssi_avg = -61, .rssi_last = -63, .link_margin_db = 39, .frame_err = 0x0100,
        .msg_err = 0x0200, .version = 4, .csl = true,
    };
}

static mesh_scan_view_t view(void)
{
    mesh_scan_view_t v;

    mesh_scan_view(&v);
    return v;
}

static void start(void)
{
    fake_ops_install();
    TEST_ASSERT_EQUAL(ESP_OK, mesh_scan_core_start());
}

/** Start, report the given routers, end discover. */
static void discover(const uint8_t *ids, size_t n)
{
    start();
    for (size_t i = 0; i < n; i++) {
        mesh_scan_on_router(router(ids[i], 0));
    }
    mesh_scan_on_discover_done(MESH_SCAN_OT_ERROR_NONE);
}

/** Answer both queries of the router being queried with success. */
static void answer_router(void)
{
    mesh_scan_on_child_table_done(MESH_SCAN_OT_ERROR_NONE);
    mesh_scan_on_child_addrs_done(MESH_SCAN_OT_ERROR_NONE);
}

static const mesh_scan_result_t *done_result(void)
{
    mesh_scan_view_t v = view();

    TEST_ASSERT_EQUAL(MESH_SCAN_DONE, v.state);
    TEST_ASSERT_NOT_NULL(v.result);
    return v.result;
}

static void assert_failed(mesh_scan_error_t error)
{
    mesh_scan_view_t v = view();

    TEST_ASSERT_EQUAL(MESH_SCAN_FAILED, v.state);
    TEST_ASSERT_EQUAL(error, v.error);
}

// mesh_scan_start

TEST_CASE("When idle, done or failed and the mode is BR_RUNNING: frees the previous result, allocates a new one, "
          "calls ops.discover, state becomes running", "[mesh_scan]")
{
    // idle
    fake_ops_install();
    mesh_scan_core_start();
    TEST_ASSERT_EQUAL(1, g_fake.allocs);
    TEST_ASSERT_EQUAL(1, g_fake.discover_calls);
    TEST_ASSERT_EQUAL(1, g_fake.arm_calls);
    TEST_ASSERT_EQUAL(MESH_SCAN_CAP_MS, g_fake.armed_ms);
    TEST_ASSERT_EQUAL(MESH_SCAN_RUNNING, view().state);

    // done, with a children array: the result and the array are released
    mesh_scan_on_router(router(3, 2));
    mesh_scan_on_discover_done(MESH_SCAN_OT_ERROR_NONE);
    answer_router();
    done_result();
    TEST_ASSERT_EQUAL(2, g_fake.allocs);
    mesh_scan_core_start();
    TEST_ASSERT_EQUAL(2, g_fake.releases);
    TEST_ASSERT_EQUAL(3, g_fake.allocs);
    TEST_ASSERT_EQUAL(2, g_fake.discover_calls);
    TEST_ASSERT_EQUAL(MESH_SCAN_RUNNING, view().state);

    // failed
    mesh_scan_core_cancel();
    TEST_ASSERT_EQUAL(MESH_SCAN_FAILED, view().state);
    mesh_scan_core_start();
    TEST_ASSERT_EQUAL(3, g_fake.releases);
    TEST_ASSERT_EQUAL(4, g_fake.allocs);
    TEST_ASSERT_EQUAL(3, g_fake.discover_calls);
    TEST_ASSERT_EQUAL(MESH_SCAN_RUNNING, view().state);
}

TEST_CASE("When idle, done or failed and the mode is BR_RUNNING: returns ESP_OK", "[mesh_scan]")
{
    fake_ops_install();
    TEST_ASSERT_EQUAL(ESP_OK, mesh_scan_core_start());
    mesh_scan_on_discover_done(MESH_SCAN_OT_ERROR_NONE);
    TEST_ASSERT_EQUAL(ESP_OK, mesh_scan_core_start());
    mesh_scan_core_cancel();
    TEST_ASSERT_EQUAL(ESP_OK, mesh_scan_core_start());
}

TEST_CASE("When a scan is running: returns ESP_ERR_INVALID_STATE and leaves the scan untouched", "[mesh_scan]")
{
    start();
    mesh_scan_on_router(router(3, 0));
    mesh_scan_on_discover_done(MESH_SCAN_OT_ERROR_NONE);
    const mesh_scan_view_t before = view();

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, mesh_scan_core_start());
    const mesh_scan_view_t after = view();
    TEST_ASSERT_EQUAL(MESH_SCAN_RUNNING, after.state);
    TEST_ASSERT_EQUAL(before.routers, after.routers);
    TEST_ASSERT_EQUAL(1, g_fake.allocs);
    TEST_ASSERT_EQUAL(0, g_fake.releases);
    TEST_ASSERT_EQUAL(1, g_fake.discover_calls);
    TEST_ASSERT_EQUAL(0, g_fake.cancel_calls);

    // and the scan carries on
    answer_router();
    TEST_ASSERT_EQUAL(MESH_SCAN_DONE, view().state);
}

TEST_CASE("When ops.discover returns OT_ERROR_BUSY: state becomes failed with error busy", "[mesh_scan]")
{
    fake_ops_install();
    g_fake.discover_ret = MESH_SCAN_OT_ERROR_BUSY;
    mesh_scan_core_start();
    assert_failed(MESH_SCAN_ERR_BUSY);
    TEST_ASSERT_EQUAL(1, g_fake.disarm_calls);
}

TEST_CASE("When ops.discover returns OT_ERROR_BUSY: returns ESP_ERR_NOT_FINISHED", "[mesh_scan]")
{
    fake_ops_install();
    g_fake.discover_ret = MESH_SCAN_OT_ERROR_BUSY;
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FINISHED, mesh_scan_core_start());
}

TEST_CASE("When ops.discover returns another error: state becomes failed with error stopped and returns ESP_FAIL",
          "[mesh_scan]")
{
    fake_ops_install();
    g_fake.discover_ret = MESH_SCAN_OT_ERROR_INVALID_STATE;
    TEST_ASSERT_EQUAL(ESP_FAIL, mesh_scan_core_start());
    assert_failed(MESH_SCAN_ERR_STOPPED);
}

TEST_CASE("When the result cannot be allocated: state becomes failed with error no_memory", "[mesh_scan]")
{
    fake_ops_install();
    g_fake.fail_alloc_at = 1;
    mesh_scan_core_start();
    assert_failed(MESH_SCAN_ERR_NO_MEMORY);
    TEST_ASSERT_EQUAL(0, g_fake.discover_calls);
}

TEST_CASE("When the result cannot be allocated: returns ESP_ERR_NO_MEM", "[mesh_scan]")
{
    fake_ops_install();
    g_fake.fail_alloc_at = 1;
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, mesh_scan_core_start());
}

TEST_CASE("When the mode is not BR_RUNNING: returns ESP_ERR_INVALID_STATE without calling ops", "[mesh_scan]")
{
    fake_ops_install();
    g_fake.br_running = false;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, mesh_scan_core_start());
    TEST_ASSERT_EQUAL(0, g_fake.ops_calls);
    TEST_ASSERT_EQUAL(MESH_SCAN_IDLE, view().state);
}

// discover callback

TEST_CASE("On each router reported: stores ext address, rloc16, router id, version, the three flags, link qualities, "
          "addresses (up to the cap) and children (rloc16, mode, lq_in)", "[mesh_scan]")
{
    start();
    mesh_scan_router_t r = *router(9, 2);
    memcpy(r.ext, "\x01\x02\x03\x04\x05\x06\x07\x08", 8);
    r.leader = true;
    r.this_device = true;
    r.border_router = true;
    r.lq_in[3] = 2;
    r.lq_in[34] = 3;
    r.n_addrs = MESH_SCAN_MAX_ROUTER_ADDRS;
    for (int i = 0; i < MESH_SCAN_MAX_ROUTER_ADDRS; i++) {
        r.addrs[i][0] = 0xfd;
        r.addrs[i][15] = (uint8_t)i;
    }
    s_children[0].full_thread_device = true;
    s_children[0].rx_on_when_idle = true;
    s_children[0].full_network_data = true;
    s_children[0].lq_in = 3;
    mesh_scan_on_router(&r);
    mesh_scan_on_discover_done(MESH_SCAN_OT_ERROR_NONE);
    answer_router();

    const mesh_scan_router_t *got = &done_result()->routers[9];
    TEST_ASSERT_TRUE(got->present);
    TEST_ASSERT_TRUE(got->responded);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(r.ext, got->ext, 8);
    TEST_ASSERT_EQUAL_HEX16(RLOC16(9), got->rloc16);
    TEST_ASSERT_EQUAL(9, got->router_id);
    TEST_ASSERT_EQUAL(5, got->version);
    TEST_ASSERT_TRUE(got->leader);
    TEST_ASSERT_TRUE(got->this_device);
    TEST_ASSERT_TRUE(got->border_router);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(r.lq_in, got->lq_in, MESH_SCAN_MAX_ROUTERS);
    TEST_ASSERT_EQUAL(MESH_SCAN_MAX_ROUTER_ADDRS, got->n_addrs);
    TEST_ASSERT_EQUAL_MEMORY(r.addrs, got->addrs, sizeof(r.addrs));
    TEST_ASSERT_EQUAL(2, got->n_children);
    TEST_ASSERT_NOT_NULL(got->children);
    TEST_ASSERT_EQUAL_HEX16(RLOC16(9) + 1, got->children[0].rloc16);
    TEST_ASSERT_TRUE(got->children[0].full_thread_device);
    TEST_ASSERT_TRUE(got->children[0].rx_on_when_idle);
    TEST_ASSERT_TRUE(got->children[0].full_network_data);
    TEST_ASSERT_EQUAL(3, got->children[0].lq_in);
    TEST_ASSERT_FALSE(got->children[0].has_ext);
    TEST_ASSERT_EQUAL_HEX16(RLOC16(9) + 2, got->children[1].rloc16);
    TEST_ASSERT_FALSE(got->children[1].full_thread_device);
    TEST_ASSERT_EQUAL(2, got->children[1].lq_in);
}

TEST_CASE("On discover finished: adds a responded:false stub for every router id in ops.router_table that did not "
          "report", "[mesh_scan]")
{
    start();
    g_fake.table_n = 3;
    g_fake.table_ids[0] = 3;
    g_fake.table_rloc16[0] = RLOC16(3);
    g_fake.table_ids[1] = 25;
    g_fake.table_rloc16[1] = RLOC16(25);
    g_fake.table_ids[2] = 50;
    g_fake.table_rloc16[2] = RLOC16(50);
    mesh_scan_on_router(router(3, 0));
    mesh_scan_on_discover_done(MESH_SCAN_OT_ERROR_NONE);
    TEST_ASSERT_EQUAL(1, g_fake.router_table_calls);
    TEST_ASSERT_EQUAL(1, view().routers);
    answer_router();

    const mesh_scan_result_t *res = done_result();
    TEST_ASSERT_TRUE(res->routers[3].responded);
    for (int i = 1; i < 3; i++) {
        const mesh_scan_router_t *stub = &res->routers[g_fake.table_ids[i]];
        TEST_ASSERT_TRUE(stub->present);
        TEST_ASSERT_FALSE(stub->responded);
        TEST_ASSERT_EQUAL(g_fake.table_ids[i], stub->router_id);
        TEST_ASSERT_EQUAL_HEX16(g_fake.table_rloc16[i], stub->rloc16);
        TEST_ASSERT_NULL(stub->children);
        TEST_ASSERT_EQUAL(0, stub->n_children);
    }
    TEST_ASSERT_EQUAL(1, g_fake.child_table_calls); // stubs are not queried
}

TEST_CASE("On discover finished: starts the child-table query of the lowest router id", "[mesh_scan]")
{
    const uint8_t ids[] = { 34, 9, 3 };

    discover(ids, 3);
    TEST_ASSERT_EQUAL(1, g_fake.child_table_calls);
    TEST_ASSERT_EQUAL_HEX16(RLOC16(3), g_fake.child_table_rloc16[0]);
    TEST_ASSERT_EQUAL(3, view().routers);
    TEST_ASSERT_EQUAL(0, view().queried);
}

TEST_CASE("On discover finished with no router: goes straight to the snapshot and finishes done", "[mesh_scan]")
{
    discover(NULL, 0);
    TEST_ASSERT_EQUAL(0, g_fake.child_table_calls);
    TEST_ASSERT_EQUAL(1, g_fake.snapshot_calls);
    TEST_ASSERT_EQUAL(1, g_fake.disarm_calls);
    TEST_ASSERT_FALSE(done_result()->partial);
}

// per-router queries

TEST_CASE("On a child-table entry: fills that child's ext address, timeout, age, connection time, RSSI avg/last, link "
          "margin, error rates, version, csl", "[mesh_scan]")
{
    start();
    mesh_scan_on_router(router(3, 1)); // child RLOC16(3) + 1: sleepy, lq_in 2
    mesh_scan_on_discover_done(MESH_SCAN_OT_ERROR_NONE);
    const mesh_scan_child_t entry = table_entry(RLOC16(3) + 1);
    mesh_scan_on_child_entry(&entry);
    answer_router();

    const mesh_scan_router_t *r = &done_result()->routers[3];
    TEST_ASSERT_EQUAL(1, r->n_children);
    const mesh_scan_child_t *c = &r->children[0];
    TEST_ASSERT_TRUE(c->has_ext);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(entry.ext, c->ext, 8);
    TEST_ASSERT_EQUAL(240, c->timeout_s);
    TEST_ASSERT_EQUAL(12, c->age_s);
    TEST_ASSERT_EQUAL(86400, c->connected_s);
    TEST_ASSERT_EQUAL(-61, c->rssi_avg);
    TEST_ASSERT_EQUAL(-63, c->rssi_last);
    TEST_ASSERT_EQUAL(39, c->link_margin_db);
    TEST_ASSERT_EQUAL_HEX16(0x0100, c->frame_err);
    TEST_ASSERT_EQUAL_HEX16(0x0200, c->msg_err);
    TEST_ASSERT_EQUAL(4, c->version);
    TEST_ASSERT_TRUE(c->csl);
    // discover's mode and lq_in stay
    TEST_ASSERT_FALSE(c->rx_on_when_idle);
    TEST_ASSERT_FALSE(c->full_thread_device);
    TEST_ASSERT_EQUAL(2, c->lq_in);
    TEST_ASSERT_TRUE(r->children_detail);
}

TEST_CASE("On a child-table entry for an RLOC16 discover did not list: adds the child with the table data and mode "
          "from the entry", "[mesh_scan]")
{
    start();
    mesh_scan_on_router(router(3, 1));
    mesh_scan_on_discover_done(MESH_SCAN_OT_ERROR_NONE);
    const mesh_scan_child_t entry = table_entry(RLOC16(3) + 7);
    mesh_scan_on_child_entry(&entry);
    answer_router();

    const mesh_scan_router_t *r = &done_result()->routers[3];
    TEST_ASSERT_EQUAL(2, r->n_children);
    const mesh_scan_child_t *c = &r->children[1];
    TEST_ASSERT_EQUAL_HEX16(RLOC16(3) + 7, c->rloc16);
    TEST_ASSERT_TRUE(c->has_ext);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(entry.ext, c->ext, 8);
    TEST_ASSERT_EQUAL(240, c->timeout_s);
    TEST_ASSERT_EQUAL(-61, c->rssi_avg);
    TEST_ASSERT_TRUE(c->csl);
    TEST_ASSERT_TRUE(c->rx_on_when_idle);
    TEST_ASSERT_TRUE(c->full_thread_device);
    TEST_ASSERT_TRUE(c->full_network_data);
    TEST_ASSERT_EQUAL(0, c->n_addrs);
}

TEST_CASE("On the child-table query done: starts the children-addresses query of the same router", "[mesh_scan]")
{
    const uint8_t ids[] = { 3, 9 };

    discover(ids, 2);
    mesh_scan_on_child_table_done(MESH_SCAN_OT_ERROR_NONE);
    TEST_ASSERT_EQUAL(1, g_fake.child_addrs_calls);
    TEST_ASSERT_EQUAL_HEX16(RLOC16(3), g_fake.child_addrs_rloc16[0]);
    TEST_ASSERT_EQUAL(1, g_fake.child_table_calls);
}

TEST_CASE("On a children-addresses report: stores the addresses of the child with that RLOC16 (up to the cap)",
          "[mesh_scan]")
{
    uint8_t addrs[MESH_SCAN_MAX_CHILD_ADDRS + 2][16] = { 0 };

    for (int i = 0; i < MESH_SCAN_MAX_CHILD_ADDRS + 2; i++) {
        addrs[i][0] = 0xfd;
        addrs[i][15] = (uint8_t)i;
    }
    start();
    mesh_scan_on_router(router(3, 2));
    mesh_scan_on_discover_done(MESH_SCAN_OT_ERROR_NONE);
    mesh_scan_on_child_table_done(MESH_SCAN_OT_ERROR_NONE);
    mesh_scan_on_child_addrs(RLOC16(3) + 2, (const uint8_t (*)[16])addrs, MESH_SCAN_MAX_CHILD_ADDRS + 2);
    mesh_scan_on_child_addrs_done(MESH_SCAN_OT_ERROR_NONE);

    const mesh_scan_router_t *r = &done_result()->routers[3];
    TEST_ASSERT_EQUAL(0, r->children[0].n_addrs);
    TEST_ASSERT_EQUAL(MESH_SCAN_MAX_CHILD_ADDRS, r->children[1].n_addrs);
    TEST_ASSERT_EQUAL_MEMORY(addrs, r->children[1].addrs, sizeof(r->children[1].addrs));
}

TEST_CASE("On the addresses query done: increments progress.queried and starts the next router, or the snapshot after "
          "the last", "[mesh_scan]")
{
    const uint8_t ids[] = { 9, 3 };

    discover(ids, 2);
    answer_router();
    TEST_ASSERT_EQUAL(1, view().queried);
    TEST_ASSERT_EQUAL(2, g_fake.child_table_calls);
    TEST_ASSERT_EQUAL_HEX16(RLOC16(9), g_fake.child_table_rloc16[1]);
    TEST_ASSERT_EQUAL(0, g_fake.snapshot_calls);
    TEST_ASSERT_EQUAL(MESH_SCAN_RUNNING, view().state);

    answer_router();
    TEST_ASSERT_EQUAL(1, g_fake.snapshot_calls);
    TEST_ASSERT_FALSE(done_result()->partial);
}

TEST_CASE("On a query timing out or answering with an error: marks that router children_detail:false and moves on",
          "[mesh_scan]")
{
    const uint8_t ids[] = { 3, 9, 34 };

    discover(ids, 3);
    mesh_scan_on_child_table_done(MESH_SCAN_OT_ERROR_RESPONSE_TIMEOUT); // router 3: no addresses query
    TEST_ASSERT_EQUAL(0, g_fake.child_addrs_calls);
    TEST_ASSERT_EQUAL_HEX16(RLOC16(9), g_fake.child_table_rloc16[1]);
    mesh_scan_on_child_table_done(MESH_SCAN_OT_ERROR_NONE); // router 9: the addresses query answers an error
    mesh_scan_on_child_addrs_done(MESH_SCAN_OT_ERROR_RESPONSE_TIMEOUT);
    TEST_ASSERT_EQUAL_HEX16(RLOC16(34), g_fake.child_table_rloc16[2]);
    TEST_ASSERT_EQUAL(2, view().queried);
    answer_router(); // router 34

    const mesh_scan_result_t *res = done_result();
    TEST_ASSERT_FALSE(res->routers[3].children_detail);
    TEST_ASSERT_FALSE(res->routers[9].children_detail);
    TEST_ASSERT_TRUE(res->routers[34].children_detail);
    TEST_ASSERT_TRUE(res->partial);
}

TEST_CASE("On a query that fails to start with OT_ERROR_NO_BUFS or OT_ERROR_BUSY: marks that router "
          "children_detail:false and moves on", "[mesh_scan]")
{
    const uint8_t ids[] = { 3, 9 };

    // the child-table query
    fake_ops_install();
    g_fake.child_table_ret = MESH_SCAN_OT_ERROR_NO_BUFS;
    mesh_scan_core_start();
    mesh_scan_on_router(router(3, 0));
    mesh_scan_on_router(router(9, 0));
    mesh_scan_on_discover_done(MESH_SCAN_OT_ERROR_NONE);
    TEST_ASSERT_EQUAL(2, g_fake.child_table_calls);
    const mesh_scan_result_t *res = done_result();
    TEST_ASSERT_FALSE(res->routers[3].children_detail);
    TEST_ASSERT_FALSE(res->routers[9].children_detail);

    // the addresses query
    discover(ids, 2);
    g_fake.child_addrs_ret = MESH_SCAN_OT_ERROR_BUSY;
    mesh_scan_on_child_table_done(MESH_SCAN_OT_ERROR_NONE);
    TEST_ASSERT_EQUAL_HEX16(RLOC16(9), g_fake.child_table_rloc16[1]);
    g_fake.child_addrs_ret = MESH_SCAN_OT_ERROR_NONE;
    answer_router();
    res = done_result();
    TEST_ASSERT_FALSE(res->routers[3].children_detail);
    TEST_ASSERT_TRUE(res->routers[9].children_detail);
}

TEST_CASE("On a query that fails to start with OT_ERROR_INVALID_STATE: state becomes failed with error stopped",
          "[mesh_scan]")
{
    const uint8_t ids[] = { 3 };

    fake_ops_install();
    g_fake.child_table_ret = MESH_SCAN_OT_ERROR_INVALID_STATE;
    mesh_scan_core_start();
    mesh_scan_on_router(router(3, 0));
    mesh_scan_on_discover_done(MESH_SCAN_OT_ERROR_NONE);
    assert_failed(MESH_SCAN_ERR_STOPPED);

    discover(ids, 1);
    g_fake.child_addrs_ret = MESH_SCAN_OT_ERROR_INVALID_STATE;
    mesh_scan_on_child_table_done(MESH_SCAN_OT_ERROR_NONE);
    assert_failed(MESH_SCAN_ERR_STOPPED);
    TEST_ASSERT_EQUAL(0, g_fake.snapshot_calls);
}

TEST_CASE("On more children than MESH_SCAN_MAX_CHILDREN for a router: keeps the first MESH_SCAN_MAX_CHILDREN and "
          "marks the router children_detail:false", "[mesh_scan]")
{
    // child-table entries (Review Focus 3): 130 entries for one router
    start();
    mesh_scan_on_router(router(3, 0));
    mesh_scan_on_discover_done(MESH_SCAN_OT_ERROR_NONE);
    for (uint16_t i = 0; i < MESH_SCAN_MAX_CHILDREN + 2; i++) {
        const mesh_scan_child_t entry = table_entry((uint16_t)(RLOC16(3) + 1 + i));
        mesh_scan_on_child_entry(&entry);
    }
    answer_router();
    const mesh_scan_result_t *res = done_result();
    TEST_ASSERT_EQUAL(MESH_SCAN_MAX_CHILDREN, res->routers[3].n_children);
    TEST_ASSERT_EQUAL_HEX16(RLOC16(3) + MESH_SCAN_MAX_CHILDREN, res->routers[3].children[MESH_SCAN_MAX_CHILDREN - 1].rloc16);
    TEST_ASSERT_FALSE(res->routers[3].children_detail);
    TEST_ASSERT_TRUE(res->partial);

    // discover reporting more children than the cap
    start();
    mesh_scan_on_router(router(3, MESH_SCAN_MAX_CHILDREN + 2));
    mesh_scan_on_discover_done(MESH_SCAN_OT_ERROR_NONE);
    answer_router();
    res = done_result();
    TEST_ASSERT_EQUAL(MESH_SCAN_MAX_CHILDREN, res->routers[3].n_children);
    TEST_ASSERT_FALSE(res->routers[3].children_detail);
    TEST_ASSERT_TRUE(res->partial);
}

// snapshot

TEST_CASE("On reaching the snapshot: copies the network fields, the SLAAC on-mesh prefixes and the SRP hosts",
          "[mesh_scan]")
{
    const uint8_t ids[] = { 3 };

    discover(ids, 1);
    answer_router();
    const mesh_scan_result_t *res = done_result();
    TEST_ASSERT_EQUAL(1, g_fake.snapshot_calls);
    TEST_ASSERT_EQUAL_STRING("OpenThread-test", res->network.name);
    TEST_ASSERT_EQUAL(15, res->network.channel);
    TEST_ASSERT_EQUAL_HEX32(0x12345678, res->network.partition_id);
    TEST_ASSERT_EQUAL(9, res->network.leader_router_id);
    TEST_ASSERT_EQUAL_HEX16(0x8800, res->network.this_rloc16);
    TEST_ASSERT_EQUAL(1, res->network.n_omr);
    TEST_ASSERT_EQUAL_HEX8(0xfe, res->network.omr[0][0]);
    TEST_ASSERT_EQUAL(1, res->n_srp);
    TEST_ASSERT_EQUAL_STRING("lamp", res->srp[0].name);
    TEST_ASSERT_TRUE(res->routers[3].present); // the snapshot keeps the routers
}

TEST_CASE("On reaching the snapshot: sets partial per the global rule, records the end time, state becomes done",
          "[mesh_scan]")
{
    const uint8_t ids[] = { 3 };

    // everything answered
    discover(ids, 1);
    g_fake.now_ms = 5000;
    answer_router();
    TEST_ASSERT_FALSE(done_result()->partial);
    TEST_ASSERT_EQUAL(1, g_fake.disarm_calls);
    g_fake.now_ms = 8500;
    TEST_ASSERT_EQUAL(3, view().age_s);

    // a stub
    start();
    g_fake.table_n = 1;
    g_fake.table_ids[0] = 25;
    g_fake.table_rloc16[0] = RLOC16(25);
    mesh_scan_on_discover_done(MESH_SCAN_OT_ERROR_NONE);
    TEST_ASSERT_TRUE(done_result()->partial);

    // a router without children detail
    discover(ids, 1);
    mesh_scan_on_child_table_done(MESH_SCAN_OT_ERROR_RESPONSE_TIMEOUT);
    TEST_ASSERT_TRUE(done_result()->partial);
}

// mesh_scan_cancel

TEST_CASE("While running: calls ops.cancel, state becomes failed with error stopped", "[mesh_scan]")
{
    start();
    mesh_scan_core_cancel();
    TEST_ASSERT_EQUAL(1, g_fake.cancel_calls);
    TEST_ASSERT_EQUAL(1, g_fake.disarm_calls);
    assert_failed(MESH_SCAN_ERR_STOPPED);

    // late callbacks of the cancelled scan are ignored
    mesh_scan_on_discover_done(MESH_SCAN_OT_ERROR_NONE);
    assert_failed(MESH_SCAN_ERR_STOPPED);
    TEST_ASSERT_EQUAL(0, g_fake.child_table_calls);
}

TEST_CASE("While not running: mesh_scan_cancel does nothing", "[mesh_scan]")
{
    fake_ops_install();
    mesh_scan_core_cancel();
    TEST_ASSERT_EQUAL(MESH_SCAN_IDLE, view().state);

    discover(NULL, 0);
    const int calls = g_fake.ops_calls;
    mesh_scan_core_cancel();
    TEST_ASSERT_EQUAL(calls, g_fake.ops_calls);
    TEST_ASSERT_EQUAL(0, g_fake.cancel_calls);
    TEST_ASSERT_EQUAL(MESH_SCAN_DONE, view().state);
}

// cap timer

TEST_CASE("On 60 s elapsed while running and the mode is BR_RUNNING: calls ops.cancel, takes the snapshot with "
          "partial:true, state becomes done", "[mesh_scan]")
{
    const uint8_t ids[] = { 3, 9, 34 };

    discover(ids, 3);
    answer_router();                                        // router 3 finished
    mesh_scan_on_child_table_done(MESH_SCAN_OT_ERROR_NONE); // router 9 cut off in its addresses query
    g_fake.now_ms += MESH_SCAN_CAP_MS;
    mesh_scan_on_cap();
    TEST_ASSERT_EQUAL(1, g_fake.cancel_calls);
    TEST_ASSERT_EQUAL(1, g_fake.snapshot_calls);
    const mesh_scan_result_t *res = done_result();
    TEST_ASSERT_TRUE(res->partial);
    TEST_ASSERT_TRUE(res->routers[3].children_detail);
    TEST_ASSERT_FALSE(res->routers[9].children_detail);  // cut off
    TEST_ASSERT_FALSE(res->routers[34].children_detail); // not reached
}

TEST_CASE("On 60 s elapsed during discover and the mode is BR_RUNNING: finishes done with children_detail:false on "
          "every responding router", "[mesh_scan]")
{
    start();
    mesh_scan_on_router(router(3, 1));
    mesh_scan_on_router(router(9, 0));
    mesh_scan_on_cap();
    TEST_ASSERT_EQUAL(1, g_fake.cancel_calls);
    TEST_ASSERT_EQUAL(0, g_fake.child_table_calls);
    const mesh_scan_result_t *res = done_result();
    TEST_ASSERT_TRUE(res->partial);
    TEST_ASSERT_FALSE(res->routers[3].children_detail);
    TEST_ASSERT_FALSE(res->routers[9].children_detail);
    TEST_ASSERT_EQUAL(1, res->routers[3].n_children); // discover data stays
}

TEST_CASE("On 60 s elapsed while the mode is not BR_RUNNING: state becomes failed with error stopped", "[mesh_scan]")
{
    start();
    g_fake.br_running = false;
    mesh_scan_on_cap();
    assert_failed(MESH_SCAN_ERR_STOPPED);
    TEST_ASSERT_EQUAL(0, g_fake.snapshot_calls);
}

// any callback

typedef void (*callback_t)(void);

static void cb_router(void)
{
    mesh_scan_on_router(router(9, 0));
}

static void cb_discover_done(void)
{
    mesh_scan_on_discover_done(MESH_SCAN_OT_ERROR_NONE);
}

static void cb_child_entry(void)
{
    const mesh_scan_child_t entry = table_entry(RLOC16(3) + 1);
    mesh_scan_on_child_entry(&entry);
}

static void cb_child_table_done(void)
{
    mesh_scan_on_child_table_done(MESH_SCAN_OT_ERROR_NONE);
}

static void cb_child_addrs(void)
{
    const uint8_t addr[1][16] = { { 0xfd } };
    mesh_scan_on_child_addrs(RLOC16(3) + 1, addr, 1);
}

static void cb_child_addrs_done(void)
{
    mesh_scan_on_child_addrs_done(MESH_SCAN_OT_ERROR_NONE);
}

TEST_CASE("When the mode is no longer BR_RUNNING: any callback calls ops.cancel, state becomes failed with error "
          "stopped", "[mesh_scan]")
{
    const callback_t callbacks[] = { cb_router, cb_discover_done, cb_child_entry, cb_child_table_done,
                                     cb_child_addrs, cb_child_addrs_done
                                   };
    const uint8_t ids[] = { 3 };

    for (size_t i = 0; i < sizeof(callbacks) / sizeof(callbacks[0]); i++) {
        if (i < 2) {
            start(); // discover phase
        } else {
            discover(ids, 1); // child-table phase
            if (i >= 4) {
                mesh_scan_on_child_table_done(MESH_SCAN_OT_ERROR_NONE); // addresses phase
            }
        }
        g_fake.br_running = false;
        callbacks[i]();
        TEST_ASSERT_EQUAL_MESSAGE(1, g_fake.cancel_calls, "callback index");
        assert_failed(MESH_SCAN_ERR_STOPPED);
        TEST_ASSERT_EQUAL(0, g_fake.snapshot_calls);
    }
}

// age_s

TEST_CASE("After done or failed: age_s is whole seconds from the end time to ops.now_ms", "[mesh_scan]")
{
    discover(NULL, 0); // done at 1000 ms
    g_fake.now_ms = 3999;
    TEST_ASSERT_EQUAL(2, view().age_s);

    start();
    g_fake.now_ms = 10000;
    mesh_scan_core_cancel();
    g_fake.now_ms = 10999;
    TEST_ASSERT_EQUAL(0, view().age_s);
    g_fake.now_ms = 71000;
    TEST_ASSERT_EQUAL(61, view().age_s);
}

// mesh_scan_render (ruling R2)

typedef struct {
    char text[4096];
    size_t len;
    int releases_inside;
    esp_err_t start_inside;
    bool started;
} render_sink_t;

static esp_err_t render_sink(void *ctx, const char *data, size_t len)
{
    render_sink_t *s = ctx;

    if (!s->started) {
        // a new scan starts while this render still reads the old result
        s->started = true;
        s->start_inside = mesh_scan_core_start();
        s->releases_inside = g_fake.releases;
    }
    if (s->len + len < sizeof(s->text)) {
        memcpy(s->text + s->len, data, len);
        s->len += len;
        s->text[s->len] = '\0';
    }
    return ESP_OK;
}

static esp_err_t text_sink(void *ctx, const char *data, size_t len)
{
    render_sink_t *s = ctx;

    TEST_ASSERT_LESS_THAN(sizeof(s->text), s->len + len);
    memcpy(s->text + s->len, data, len);
    s->len += len;
    s->text[s->len] = '\0';
    return ESP_OK;
}

TEST_CASE("On a render taken during done: keeps its result alive across a new start, freed on the last release",
          "[mesh_scan]")
{
    start();
    mesh_scan_on_router(router(3, 2)); // the result and one children array
    mesh_scan_on_discover_done(MESH_SCAN_OT_ERROR_NONE);
    answer_router();
    TEST_ASSERT_EQUAL(MESH_SCAN_DONE, view().state);
    TEST_ASSERT_EQUAL(0, g_fake.releases);

    render_sink_t s = { 0 };
    TEST_ASSERT_EQUAL(ESP_OK, mesh_scan_render(render_sink, &s));
    TEST_ASSERT_EQUAL(ESP_OK, s.start_inside);
    TEST_ASSERT_EQUAL(0, s.releases_inside);
    TEST_ASSERT_EQUAL(2, g_fake.releases);
    TEST_ASSERT_EQUAL(MESH_SCAN_RUNNING, view().state);
    TEST_ASSERT_EQUAL_STRING_LEN("{\"state\":\"done\"", s.text, 15);
}

TEST_CASE("On a render while running: emits progress only", "[mesh_scan]")
{
    const uint8_t ids[] = { 3, 9 };
    render_sink_t s = { 0 };

    discover(ids, 2);
    answer_router();
    TEST_ASSERT_EQUAL(ESP_OK, mesh_scan_render(text_sink, &s));
    TEST_ASSERT_EQUAL_STRING("{\"state\":\"running\",\"progress\":{\"routers\":2,\"queried\":1}}", s.text);
}
