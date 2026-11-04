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
    s_router.has_version = true;
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
        .has_err_rate = true, .msg_err = 0x0200, .version = 4, .csl = true,
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

/** A discover report for router `id` with `n` rx-on full-device children and no addresses (RLOC16 as router()). */
static const mesh_scan_router_t *fed_router(uint8_t id, uint16_t n)
{
    router(id, n);
    for (uint16_t i = 0; i < n; i++) {
        s_children[i].rx_on_when_idle = true;
        s_children[i].full_thread_device = true;
    }
    return &s_router;
}

/** Start and run router 3, with `n` rx-on children without addresses, up to the child address step. */
static void child_step(uint16_t n)
{
    start();
    mesh_scan_on_router(fed_router(3, n));
    mesh_scan_on_discover_done(MESH_SCAN_OT_ERROR_NONE);
    answer_router();
}

/** The current scan's child `rloc16` answers with `n` addresses fd00::<rloc16>:<i>. */
static void answer_child(uint16_t rloc16, uint8_t n)
{
    uint8_t addrs[MESH_SCAN_MAX_CHILD_ADDRS + 2][16] = { 0 };

    for (uint8_t i = 0; i < n && i < MESH_SCAN_MAX_CHILD_ADDRS + 2; i++) {
        addrs[i][0] = 0xfd;
        addrs[i][13] = (uint8_t)(rloc16 >> 8);
        addrs[i][14] = (uint8_t)rloc16;
        addrs[i][15] = i;
    }
    mesh_scan_on_child_diag(g_fake.armed_gen, rloc16, (const uint8_t (*)[16])addrs, n);
}

/** Let `ms` pass and deliver the child deadline of the current scan. */
static void expire(int64_t ms)
{
    g_fake.now_ms += ms;
    mesh_scan_on_child_deadline(g_fake.armed_gen);
}

/** Give up on every child query until the scan ends. */
static void drain_children(void)
{
    for (int i = 0; i < 1000 && view().state == MESH_SCAN_RUNNING; i++) {
        expire(MESH_SCAN_CHILD_DEADLINE_MS);
    }
}

static const mesh_scan_child_t *child_of(const mesh_scan_result_t *res, uint16_t rloc16)
{
    const mesh_scan_router_t *r = &res->routers[rloc16 >> 10];

    for (uint16_t i = 0; i < r->n_children; i++) {
        if (r->children[i].rloc16 == rloc16) {
            return &r->children[i];
        }
    }
    TEST_FAIL_MESSAGE("no such child");
    return NULL;
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
    TEST_ASSERT_EQUAL(2, g_fake.allocs); // the result and its pool
    TEST_ASSERT_EQUAL(1, g_fake.discover_calls);
    TEST_ASSERT_EQUAL(1, g_fake.arm_calls);
    TEST_ASSERT_EQUAL(MESH_SCAN_CAP_MS, g_fake.armed_ms);
    TEST_ASSERT_EQUAL(MESH_SCAN_RUNNING, view().state);

    // done, with a children array: the result and the array are released
    mesh_scan_on_router(router(3, 2));
    mesh_scan_on_discover_done(MESH_SCAN_OT_ERROR_NONE);
    answer_router();
    done_result();
    TEST_ASSERT_EQUAL(3, g_fake.allocs);
    mesh_scan_core_start();
    TEST_ASSERT_EQUAL(3, g_fake.releases);
    TEST_ASSERT_EQUAL(5, g_fake.allocs);
    TEST_ASSERT_EQUAL(2, g_fake.discover_calls);
    TEST_ASSERT_EQUAL(MESH_SCAN_RUNNING, view().state);

    // failed
    mesh_scan_core_cancel();
    TEST_ASSERT_EQUAL(MESH_SCAN_FAILED, view().state);
    mesh_scan_core_start();
    TEST_ASSERT_EQUAL(5, g_fake.releases);
    TEST_ASSERT_EQUAL(7, g_fake.allocs);
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
    TEST_ASSERT_EQUAL(2, g_fake.allocs);
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

TEST_CASE("When the pool cannot be allocated: state becomes failed with error no_memory", "[mesh_scan]")
{
    fake_ops_install();
    g_fake.fail_alloc_at = 2;
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, mesh_scan_core_start());
    assert_failed(MESH_SCAN_ERR_NO_MEMORY);
    TEST_ASSERT_EQUAL(0, g_fake.discover_calls);
}

TEST_CASE("When the pool cannot be allocated: releases the result block", "[mesh_scan]")
{
    fake_ops_install();
    g_fake.fail_alloc_at = 2;
    mesh_scan_core_start();
    TEST_ASSERT_EQUAL(2, g_fake.allocs);
    TEST_ASSERT_EQUAL(1, g_fake.releases);
}

TEST_CASE("On a scan whose result is released: the pool block goes with it (allocs == releases)", "[mesh_scan]")
{
    start();
    TEST_ASSERT_EQUAL(2, g_fake.allocs);
    mesh_scan_core_fail_stopped();
    TEST_ASSERT_EQUAL(g_fake.allocs, g_fake.releases);
}

TEST_CASE("When the result cannot be allocated: returns ESP_ERR_NO_MEM", "[mesh_scan]")
{
    fake_ops_install();
    g_fake.fail_alloc_at = 1;
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, mesh_scan_core_start());
}

TEST_CASE("When the mode is not BR_RUNNING: returns ESP_ERR_NOT_ALLOWED without calling ops", "[mesh_scan]")
{
    fake_ops_install();
    g_fake.br_running = false;
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_ALLOWED, mesh_scan_core_start());
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
    drain_children(); // rx-on, asked directly, no answer: the deadline ends the step

    const mesh_scan_router_t *got = &done_result()->routers[9];
    TEST_ASSERT_TRUE(got->present);
    TEST_ASSERT_TRUE(got->responded);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(r.ext, got->ext, 8);
    TEST_ASSERT_EQUAL_HEX16(RLOC16(9), got->rloc16);
    TEST_ASSERT_EQUAL(9, got->router_id);
    TEST_ASSERT_TRUE(got->has_version);
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
    TEST_ASSERT_TRUE(c->has_err_rate);
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
    drain_children(); // rx-on, asked directly, no answer: the deadline ends the step

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
    drain_children(); // the entries are rx-on without addresses
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

TEST_CASE("Without the OpenThread lock, while running: mesh_scan_core_fail_stopped fails the scan with error stopped "
          "without calling ops.cancel", "[mesh_scan]")
{
    const uint8_t ids[] = { 3 };

    discover(ids, 1); // in router 3's child-table query
    const int disarms = g_fake.disarm_calls;

    mesh_scan_core_fail_stopped();
    assert_failed(MESH_SCAN_ERR_STOPPED);
    TEST_ASSERT_EQUAL(0, g_fake.cancel_calls);
    TEST_ASSERT_EQUAL(disarms + 1, g_fake.disarm_calls);
    TEST_ASSERT_EQUAL(g_fake.allocs, g_fake.releases); // the result went with the failure

    // late callbacks of the abandoned scan are ignored
    mesh_scan_on_child_table_done(MESH_SCAN_OT_ERROR_NONE);
    assert_failed(MESH_SCAN_ERR_STOPPED);
    TEST_ASSERT_EQUAL(0, g_fake.child_addrs_calls);
}

TEST_CASE("Without the OpenThread lock, while not running: mesh_scan_core_fail_stopped does nothing", "[mesh_scan]")
{
    discover(NULL, 0);
    const int calls = g_fake.ops_calls;

    mesh_scan_core_fail_stopped();
    TEST_ASSERT_EQUAL(calls, g_fake.ops_calls);
    TEST_ASSERT_EQUAL(MESH_SCAN_DONE, view().state);
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

// mode watchdog (ruling R6)

TEST_CASE("On each start: arms the cap and the watchdog with a new generation", "[mesh_scan]")
{
    start();
    const uint32_t first = g_fake.armed_gen;

    mesh_scan_core_cancel();
    TEST_ASSERT_EQUAL(ESP_OK, mesh_scan_core_start());
    TEST_ASSERT_NOT_EQUAL(first, g_fake.armed_gen);
}

TEST_CASE("On the mode left while running (the watchdog fires): state becomes failed with error stopped, ops.cancel "
          "is not called, the cap and the watchdog are disarmed", "[mesh_scan]")
{
    const uint8_t ids[] = { 3, 9 };

    discover(ids, 2); // in router 3's child-table query
    g_fake.br_running = false;
    const int disarms = g_fake.disarm_calls;

    mesh_scan_on_mode_lost(g_fake.armed_gen);
    assert_failed(MESH_SCAN_ERR_STOPPED);
    TEST_ASSERT_EQUAL(0, g_fake.cancel_calls);
    TEST_ASSERT_EQUAL(0, g_fake.snapshot_calls);
    TEST_ASSERT_EQUAL(disarms + 1, g_fake.disarm_calls);
    TEST_ASSERT_EQUAL(g_fake.allocs, g_fake.releases); // the result went with the failure
}

TEST_CASE("On a mode-lost event of an older generation: the newer scan keeps running", "[mesh_scan]")
{
    start();
    const uint32_t old = g_fake.armed_gen;

    mesh_scan_core_cancel();
    TEST_ASSERT_EQUAL(ESP_OK, mesh_scan_core_start());
    mesh_scan_on_mode_lost(old);
    TEST_ASSERT_EQUAL(MESH_SCAN_RUNNING, view().state);
    TEST_ASSERT_EQUAL(1, g_fake.cancel_calls); // only the cancel of the first scan
}

TEST_CASE("On a mode-lost event while not running: does nothing", "[mesh_scan]")
{
    start();
    mesh_scan_on_router(router(3, 0));
    mesh_scan_on_discover_done(MESH_SCAN_OT_ERROR_NONE);
    answer_router();
    done_result();
    mesh_scan_on_mode_lost(g_fake.armed_gen);
    done_result();
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

static void cb_child_diag(void)
{
    answer_child(RLOC16(3) + 1, 1);
}

static void cb_child_deadline(void)
{
    expire(MESH_SCAN_CHILD_DEADLINE_MS);
}

TEST_CASE("When the mode is no longer BR_RUNNING: any callback fails the scan with error stopped without calling "
          "ops.cancel", "[mesh_scan]")
{
    const callback_t callbacks[] = { cb_router, cb_discover_done, cb_child_entry, cb_child_table_done,
                                     cb_child_addrs, cb_child_addrs_done, cb_child_diag, cb_child_deadline
                                   };
    const uint8_t ids[] = { 3 };

    for (size_t i = 0; i < sizeof(callbacks) / sizeof(callbacks[0]); i++) {
        if (i >= 6) {
            child_step(1); // child address step
        } else if (i < 2) {
            start(); // discover phase
        } else {
            discover(ids, 1); // child-table phase
            if (i >= 4) {
                mesh_scan_on_child_table_done(MESH_SCAN_OT_ERROR_NONE); // addresses phase
            }
        }
        g_fake.br_running = false;
        callbacks[i]();
        TEST_ASSERT_EQUAL_MESSAGE(0, g_fake.cancel_calls, "callback index"); // no otMeshDiagCancel from a callback
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
    TEST_ASSERT_EQUAL(3, g_fake.releases); // result, pool and children array
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
    TEST_ASSERT_EQUAL_STRING("{\"state\":\"running\",\"progress\":{\"routers\":2,\"queried\":1,"
                             "\"children\":{\"total\":0,\"queried\":0}}}", s.text);
}

// child address step (spec 3.4, ruling R14)

TEST_CASE("After the last router's queries, on rx-on children with no addresses: starts ops.query_child_diag for up "
          "to 4 of them, progress.children.total = their count", "[mesh_scan]")
{
    const uint8_t addr[1][16] = { { 0xfd } };

    start();
    mesh_scan_on_router(fed_router(3, 6));
    mesh_scan_on_router(fed_router(9, 1));
    mesh_scan_on_discover_done(MESH_SCAN_OT_ERROR_NONE);
    answer_router();                                         // router 3
    mesh_scan_on_child_table_done(MESH_SCAN_OT_ERROR_NONE); // router 9: its child has an address
    mesh_scan_on_child_addrs(RLOC16(9) + 1, addr, 1);
    TEST_ASSERT_EQUAL(0, g_fake.child_diag_calls);
    TEST_ASSERT_EQUAL(0, view().children_total); // before the step
    mesh_scan_on_child_addrs_done(MESH_SCAN_OT_ERROR_NONE);

    TEST_ASSERT_EQUAL(MESH_SCAN_CHILD_QUERIES, g_fake.child_diag_calls);
    for (int i = 0; i < MESH_SCAN_CHILD_QUERIES; i++) {
        TEST_ASSERT_EQUAL_HEX16(RLOC16(3) + 1 + i, g_fake.child_diag_rloc16[i]);
        TEST_ASSERT_EQUAL(g_fake.armed_gen, g_fake.child_diag_gen[i]);
    }
    TEST_ASSERT_EQUAL(1, g_fake.deadline_calls);
    TEST_ASSERT_EQUAL(MESH_SCAN_CHILD_DEADLINE_MS, g_fake.deadline_ms);
    TEST_ASSERT_EQUAL(g_fake.armed_gen, g_fake.deadline_gen);
    const mesh_scan_view_t v = view();
    TEST_ASSERT_EQUAL(MESH_SCAN_RUNNING, v.state);
    TEST_ASSERT_EQUAL(6, v.children_total);
    TEST_ASSERT_EQUAL(0, v.children_queried);
    TEST_ASSERT_EQUAL(2, v.queried);
    TEST_ASSERT_EQUAL(0, g_fake.snapshot_calls);
}

TEST_CASE("After the last router's queries, on no such child: goes straight to the snapshot", "[mesh_scan]")
{
    const uint8_t addr[1][16] = { { 0xfd } };

    start();
    mesh_scan_on_router(fed_router(3, 1));
    mesh_scan_on_discover_done(MESH_SCAN_OT_ERROR_NONE);
    mesh_scan_on_child_table_done(MESH_SCAN_OT_ERROR_NONE);
    mesh_scan_on_child_addrs(RLOC16(3) + 1, addr, 1);
    mesh_scan_on_child_addrs_done(MESH_SCAN_OT_ERROR_NONE);

    TEST_ASSERT_EQUAL(0, g_fake.child_diag_calls);
    TEST_ASSERT_EQUAL(0, g_fake.deadline_calls);
    TEST_ASSERT_EQUAL(1, g_fake.snapshot_calls);
    TEST_ASSERT_TRUE(done_result()->routers[3].children[0].addresses_reported);
}

TEST_CASE("After the last router's queries, on sleepy children with no addresses: does not ask them", "[mesh_scan]")
{
    start();
    mesh_scan_on_router(router(3, 2)); // sleepy
    mesh_scan_on_discover_done(MESH_SCAN_OT_ERROR_NONE);
    answer_router();

    TEST_ASSERT_EQUAL(0, g_fake.child_diag_calls);
    const mesh_scan_router_t *r = &done_result()->routers[3];
    TEST_ASSERT_EQUAL(0, r->children[0].n_addrs);
    TEST_ASSERT_TRUE(r->children[0].addresses_reported);
    TEST_ASSERT_TRUE(r->children[1].addresses_reported);
}

TEST_CASE("On an answer with addresses: stores them on that child (up to MESH_SCAN_MAX_CHILD_ADDRS), queried++, "
          "starts the next waiting child", "[mesh_scan]")
{
    child_step(5);
    answer_child(RLOC16(3) + 2, MESH_SCAN_MAX_CHILD_ADDRS + 2);
    TEST_ASSERT_EQUAL(1, view().children_queried);
    TEST_ASSERT_EQUAL(5, g_fake.child_diag_calls);
    TEST_ASSERT_EQUAL_HEX16(RLOC16(3) + 5, g_fake.child_diag_rloc16[4]);
    for (uint16_t rloc16 = RLOC16(3) + 1; rloc16 <= RLOC16(3) + 5; rloc16++) {
        if (rloc16 != RLOC16(3) + 2) {
            answer_child(rloc16, 1);
        }
    }

    const mesh_scan_child_t *c = child_of(done_result(), RLOC16(3) + 2);
    TEST_ASSERT_EQUAL(MESH_SCAN_MAX_CHILD_ADDRS, c->n_addrs);
    TEST_ASSERT_EQUAL_HEX8(0xfd, c->addrs[0][0]);
    TEST_ASSERT_EQUAL_HEX8(MESH_SCAN_MAX_CHILD_ADDRS - 1, c->addrs[MESH_SCAN_MAX_CHILD_ADDRS - 1][15]);
    TEST_ASSERT_TRUE(c->addresses_reported);
}

TEST_CASE("On the deadline of a query: marks that child addresses_reported false, queried++, starts the next",
          "[mesh_scan]")
{
    child_step(5); // at 1000 ms: +1..+4 in flight, due at 6000 ms
    g_fake.now_ms = 3000;
    answer_child(RLOC16(3) + 1, 1); // +5 starts, due at 8000 ms
    TEST_ASSERT_EQUAL(5, g_fake.child_diag_calls);
    TEST_ASSERT_EQUAL(3000, g_fake.deadline_ms); // the earliest query still due at 6000 ms

    expire(2999); // not due yet: an early delivery only re-arms, for the 1 ms left
    TEST_ASSERT_EQUAL(1, view().children_queried);
    TEST_ASSERT_EQUAL(1, g_fake.deadline_ms);

    expire(1); // 6000 ms
    TEST_ASSERT_EQUAL(4, view().children_queried);
    TEST_ASSERT_EQUAL(MESH_SCAN_RUNNING, view().state);
    TEST_ASSERT_EQUAL(2000, g_fake.deadline_ms); // +5 is due at 8000 ms

    expire(2000);
    const mesh_scan_result_t *res = done_result();
    TEST_ASSERT_TRUE(child_of(res, RLOC16(3) + 1)->addresses_reported);
    for (uint16_t rloc16 = RLOC16(3) + 2; rloc16 <= RLOC16(3) + 5; rloc16++) {
        TEST_ASSERT_FALSE(child_of(res, rloc16)->addresses_reported);
        TEST_ASSERT_EQUAL(0, child_of(res, rloc16)->n_addrs);
    }
    TEST_ASSERT_FALSE(res->partial); // a child's flag does not set partial
}

TEST_CASE("On an answer without an address list from a waiting child: leaves it waiting, the deadline marks it not reported",
          "[mesh_scan]")
{
    child_step(1);
    answer_child(RLOC16(3) + 1, 0); // e.g. the REST API's own query answered through the shared callback slot
    TEST_ASSERT_EQUAL(0, view().children_queried);
    TEST_ASSERT_EQUAL(MESH_SCAN_RUNNING, view().state);

    drain_children();
    const mesh_scan_child_t *c = child_of(done_result(), RLOC16(3) + 1);
    TEST_ASSERT_FALSE(c->addresses_reported);
    TEST_ASSERT_EQUAL(0, c->n_addrs);
}

TEST_CASE("On the last answer: takes the snapshot, state done", "[mesh_scan]")
{
    child_step(2);
    answer_child(RLOC16(3) + 2, 1);
    TEST_ASSERT_EQUAL(0, g_fake.snapshot_calls);
    TEST_ASSERT_EQUAL(MESH_SCAN_RUNNING, view().state);
    const int disarms = g_fake.disarm_calls;

    answer_child(RLOC16(3) + 1, 1);
    TEST_ASSERT_EQUAL(1, g_fake.snapshot_calls);
    TEST_ASSERT_EQUAL(disarms + 1, g_fake.disarm_calls);
    TEST_ASSERT_FALSE(done_result()->partial);
}

TEST_CASE("On a departed child (Review Focus 2): its query is given up at the deadline and the scan finishes",
          "[mesh_scan]")
{
    child_step(1);
    expire(MESH_SCAN_CHILD_DEADLINE_MS);
    const mesh_scan_result_t *res = done_result();
    TEST_ASSERT_FALSE(res->routers[3].children[0].addresses_reported);
    TEST_ASSERT_EQUAL(1, g_fake.snapshot_calls);
}

TEST_CASE("On a stale generation (Review Focus 1): an answer or deadline of an older scan is ignored", "[mesh_scan]")
{
    child_step(1);
    const uint32_t old = g_fake.armed_gen;
    const uint8_t addr[1][16] = { { 0xfd } };

    // cancelled, then a newer scan in the same step for the same child
    mesh_scan_core_cancel();
    TEST_ASSERT_EQUAL(ESP_OK, mesh_scan_core_start());
    mesh_scan_on_router(fed_router(3, 1));
    mesh_scan_on_discover_done(MESH_SCAN_OT_ERROR_NONE);
    answer_router();
    TEST_ASSERT_NOT_EQUAL(old, g_fake.armed_gen);
    mesh_scan_on_child_diag(old, RLOC16(3) + 1, addr, 1);
    g_fake.now_ms += MESH_SCAN_CHILD_DEADLINE_MS;
    mesh_scan_on_child_deadline(old);
    mesh_scan_view_t v = view();
    TEST_ASSERT_EQUAL(MESH_SCAN_RUNNING, v.state);
    TEST_ASSERT_EQUAL(0, v.children_queried);

    // after the cap
    mesh_scan_on_cap();
    done_result();
    answer_child(RLOC16(3) + 1, 1);
    expire(MESH_SCAN_CHILD_DEADLINE_MS);
    const mesh_scan_child_t *c = &done_result()->routers[3].children[0];
    TEST_ASSERT_EQUAL(0, c->n_addrs);
    TEST_ASSERT_FALSE(c->addresses_reported);
}

TEST_CASE("On an answer while running in another phase: is ignored", "[mesh_scan]")
{
    start();
    mesh_scan_on_router(fed_router(3, 1));
    mesh_scan_on_discover_done(MESH_SCAN_OT_ERROR_NONE); // in the child-table query
    answer_child(RLOC16(3) + 1, 1);
    expire(MESH_SCAN_CHILD_DEADLINE_MS);
    TEST_ASSERT_EQUAL(MESH_SCAN_RUNNING, view().state);

    answer_router(); // the child step asks the child
    TEST_ASSERT_EQUAL(1, g_fake.child_diag_calls);
    TEST_ASSERT_EQUAL(0, view().children_queried);
    expire(MESH_SCAN_CHILD_DEADLINE_MS);
    const mesh_scan_child_t *c = &done_result()->routers[3].children[0];
    TEST_ASSERT_EQUAL(0, c->n_addrs);
    TEST_ASSERT_FALSE(c->addresses_reported);
}

TEST_CASE("On an answer from a peer with no query in flight: is dropped", "[mesh_scan]")
{
    child_step(6); // +1..+4 in flight, +5 and +6 waiting
    answer_child(RLOC16(3) + 5, 1); // asked later, not yet
    answer_child(RLOC16(9) + 1, 1); // unknown
    TEST_ASSERT_EQUAL(0, view().children_queried);
    TEST_ASSERT_EQUAL(4, g_fake.child_diag_calls);

    answer_child(RLOC16(3) + 1, 1);
    answer_child(RLOC16(3) + 1, 1); // a second answer of the same child
    TEST_ASSERT_EQUAL(1, view().children_queried);
    drain_children();
    const mesh_scan_result_t *res = done_result();
    TEST_ASSERT_EQUAL(1, child_of(res, RLOC16(3) + 1)->n_addrs);
    TEST_ASSERT_EQUAL(0, res->routers[9].n_children);
}

TEST_CASE("On ops.query_child_diag failing to start with NO_BUFS or BUSY: marks that child not reported and moves on",
          "[mesh_scan]")
{
    fake_ops_install();
    g_fake.child_diag_rets[0] = MESH_SCAN_OT_ERROR_NO_BUFS;
    g_fake.child_diag_rets[1] = MESH_SCAN_OT_ERROR_BUSY;
    mesh_scan_core_start();
    mesh_scan_on_router(fed_router(3, 6));
    mesh_scan_on_discover_done(MESH_SCAN_OT_ERROR_NONE);
    answer_router();

    TEST_ASSERT_EQUAL(6, g_fake.child_diag_calls); // +3..+6 fill the four slots
    TEST_ASSERT_EQUAL(2, view().children_queried);
    for (uint16_t rloc16 = RLOC16(3) + 3; rloc16 <= RLOC16(3) + 6; rloc16++) {
        answer_child(rloc16, 1);
    }
    const mesh_scan_result_t *res = done_result();
    TEST_ASSERT_FALSE(child_of(res, RLOC16(3) + 1)->addresses_reported);
    TEST_ASSERT_FALSE(child_of(res, RLOC16(3) + 2)->addresses_reported);
    TEST_ASSERT_TRUE(child_of(res, RLOC16(3) + 3)->addresses_reported);

    // every start failing: the step ends at once
    fake_ops_install();
    for (int i = 0; i < 3; i++) {
        g_fake.child_diag_rets[i] = MESH_SCAN_OT_ERROR_NO_BUFS;
    }
    mesh_scan_core_start();
    mesh_scan_on_router(fed_router(3, 3));
    mesh_scan_on_discover_done(MESH_SCAN_OT_ERROR_NONE);
    answer_router();
    TEST_ASSERT_FALSE(done_result()->routers[3].children[2].addresses_reported);
}

TEST_CASE("On ops.query_child_diag failing to start with INVALID_STATE: fails the scan stopped", "[mesh_scan]")
{
    fake_ops_install();
    g_fake.child_diag_rets[1] = MESH_SCAN_OT_ERROR_INVALID_STATE;
    mesh_scan_core_start();
    mesh_scan_on_router(fed_router(3, 3));
    mesh_scan_on_discover_done(MESH_SCAN_OT_ERROR_NONE);
    answer_router();
    assert_failed(MESH_SCAN_ERR_STOPPED);
    TEST_ASSERT_EQUAL(2, g_fake.child_diag_calls);
    TEST_ASSERT_EQUAL(0, g_fake.snapshot_calls);
}

TEST_CASE("On the cap during the child address step: children still waiting are marked not reported; snapshot; done",
          "[mesh_scan]")
{
    child_step(6);
    answer_child(RLOC16(3) + 1, 1); // +5 starts; +6 not asked yet
    mesh_scan_on_cap();

    TEST_ASSERT_EQUAL(1, g_fake.cancel_calls);
    TEST_ASSERT_EQUAL(1, g_fake.snapshot_calls);
    const mesh_scan_result_t *res = done_result();
    TEST_ASSERT_TRUE(res->partial); // the cap
    TEST_ASSERT_TRUE(res->routers[3].children_detail);
    TEST_ASSERT_TRUE(child_of(res, RLOC16(3) + 1)->addresses_reported);
    TEST_ASSERT_EQUAL(1, child_of(res, RLOC16(3) + 1)->n_addrs);
    for (uint16_t rloc16 = RLOC16(3) + 2; rloc16 <= RLOC16(3) + 6; rloc16++) {
        TEST_ASSERT_FALSE(child_of(res, rloc16)->addresses_reported);
    }
    TEST_ASSERT_EQUAL(5, g_fake.child_diag_calls);
}

TEST_CASE("On the mode left during the child address step (the watchdog fires): state becomes failed with error "
          "stopped", "[mesh_scan]")
{
    child_step(2);
    g_fake.br_running = false;
    mesh_scan_on_mode_lost(g_fake.armed_gen);
    assert_failed(MESH_SCAN_ERR_STOPPED);
    TEST_ASSERT_EQUAL(0, g_fake.snapshot_calls);
}

TEST_CASE("On a render in the child address step: writes progress with children total and queried", "[mesh_scan]")
{
    render_sink_t s = { 0 };

    child_step(6);
    answer_child(RLOC16(3) + 2, 1);
    TEST_ASSERT_EQUAL(ESP_OK, mesh_scan_render(text_sink, &s));
    TEST_ASSERT_EQUAL_STRING("{\"state\":\"running\",\"progress\":{\"routers\":1,\"queried\":1,"
                             "\"children\":{\"total\":6,\"queried\":1}}}", s.text);
}

// mesh_scan_rloc16_from_peer

TEST_CASE("On an RLOC address in the mesh-local prefix: returns true and its RLOC16", "[mesh_scan]")
{
    const uint8_t prefix[8] = { 0xfd, 0xde, 0xad, 0x00, 0xbe, 0xef, 0, 0 };
    const uint8_t peer[16] = { 0xfd, 0xde, 0xad, 0x00, 0xbe, 0xef, 0, 0, 0, 0, 0, 0xff, 0xfe, 0, 0x24, 0x01 };
    uint16_t rloc16 = 0;

    TEST_ASSERT_TRUE(mesh_scan_rloc16_from_peer(peer, prefix, &rloc16));
    TEST_ASSERT_EQUAL_HEX16(0x2401, rloc16);
}

TEST_CASE("On a non-RLOC peer address or another prefix: returns false", "[mesh_scan]")
{
    const uint8_t prefix[8] = { 0xfd, 0xde, 0xad, 0x00, 0xbe, 0xef, 0, 0 };
    const uint8_t ml_eid[16] = { 0xfd, 0xde, 0xad, 0x00, 0xbe, 0xef, 0, 0, 0x1c, 0x2d, 0x3e, 0x4f, 0x5a, 0x6b, 0x24, 0x01 };
    const uint8_t other[16] = { 0xfd, 0xde, 0xad, 0x00, 0xbe, 0xee, 0, 0, 0, 0, 0, 0xff, 0xfe, 0, 0x24, 0x01 };
    const uint8_t link_local[16] = { 0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xfe, 0, 0x24, 0x01 };
    uint16_t rloc16 = 0x1234;

    TEST_ASSERT_FALSE(mesh_scan_rloc16_from_peer(ml_eid, prefix, &rloc16));
    TEST_ASSERT_FALSE(mesh_scan_rloc16_from_peer(other, prefix, &rloc16));
    TEST_ASSERT_FALSE(mesh_scan_rloc16_from_peer(link_local, prefix, &rloc16));
    TEST_ASSERT_EQUAL_HEX16(0x1234, rloc16);
}

// SRP step (Review focus 1, 2, 4)

#define THIS_BOARD 0x8800
#define REMOTE_A RLOC16(9)
#define REMOTE_B RLOC16(12)
#define SRP_TYPES 7

#define ADDR_OMR 0xfe /**< first byte of an address under the fake OMR prefix */
#define ADDR_ML 0xfd  /**< ... under the fake mesh-local prefix */
#define ADDR_LAN 0x20 /**< ... on the LAN only */

static const uint8_t EXT_PAN_ID[8] = { 0xde, 0xad, 0xbe, 0xef, 1, 2, 3, 4 };
static const uint8_t OWN_EXT[8] = { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11 };
static const uint8_t TXT[] = "\x05" "a=xyz" "\x03" "b=1";

static bool s_closed[FAKE_MAX_QUERIES]; /**< DNS calls the test has ended */
static uint8_t s_txt[1024];

static void srp_server(uint16_t rloc16, bool this_board)
{
    mesh_scan_srp_server_t *srv = &g_fake.srp[g_fake.srp_n++];

    memset(srv, 0, sizeof(*srv));
    srv->rloc16 = rloc16;
    srv->addr[0] = ADDR_ML;
    srv->addr[14] = (uint8_t)(rloc16 >> 8);
    srv->addr[15] = (uint8_t)rloc16;
    srv->port = 53;
    srv->this_board = this_board;
}

/** Install the fake with this board's server and `n_remote` remote ones (REMOTE_A, REMOTE_B), no scan started. */
static void srp_setup(int n_remote)
{
    fake_ops_install();
    memset(s_closed, 0, sizeof(s_closed));
    srp_server(THIS_BOARD, true);
    if (n_remote > 0) {
        srp_server(REMOTE_A, false);
    }
    if (n_remote > 1) {
        srp_server(REMOTE_B, false);
    }
    memcpy(g_fake.ext_pan_id, EXT_PAN_ID, 8);
    memcpy(g_fake.own_ext, OWN_EXT, 8);
}

static void srp_start(int n_remote)
{
    srp_setup(n_remote);
    TEST_ASSERT_EQUAL(ESP_OK, mesh_scan_core_start());
}

/** End the mesh phases: discover finds no router, so no child step either. */
static void mesh_over(void)
{
    mesh_scan_on_discover_done(MESH_SCAN_OT_ERROR_NONE);
}

/** A described record of `instance` on `host` with one address in the /64 of `head` bytes, TTL 120, port 5540, TXT. */
static mesh_scan_srp_record_t srp_rec(const char *instance, const char *host, uint8_t head)
{
    mesh_scan_srp_record_t rec = { .port = 5540, .n_addrs = 1, .ttl_s = 120, .txt = TXT, .txt_len = sizeof(TXT) - 1,
                                   .has_txt = true, .described = true
                                 };

    strcpy(rec.instance, instance);
    strcpy(rec.host, host);
    memset(rec.addrs[0], head, 8); // the whole /64
    rec.addrs[0][15] = 1;
    return rec;
}

static void srp_answer(int call, const mesh_scan_srp_record_t *rec)
{
    mesh_scan_on_srp_record(g_fake.dns[call].gen, g_fake.dns[call].job, rec);
}

static void srp_done(int call, int ot_error)
{
    s_closed[call] = true;
    mesh_scan_on_srp_query_done(g_fake.dns[call].gen, g_fake.dns[call].job, ot_error);
}

static int find_call(uint16_t server, const char *type, bool resolve)
{
    for (int i = 0; i < g_fake.dns_calls; i++) {
        if (g_fake.dns[i].server == server && g_fake.dns[i].resolve == resolve && !strcmp(g_fake.dns[i].type, type)) {
            return i;
        }
    }
    return -1;
}

/** The browse of `type` on `server`, ending earlier queries with NONE until it is issued. */
static int browse_of(uint16_t server, const char *type)
{
    for (;;) {
        int i = find_call(server, type, false);
        if (i >= 0) {
            return i;
        }
        int open = -1;
        for (int j = 0; j < g_fake.dns_calls && open < 0; j++) {
            open = s_closed[j] ? -1 : j;
        }
        TEST_ASSERT_TRUE_MESSAGE(open >= 0, "browse never issued");
        srp_done(open, MESH_SCAN_OT_ERROR_NONE);
    }
}

/** End every query, issued now or later, with `a_error` for REMOTE_A and NONE for the others. */
static void srp_drain(int a_error)
{
    for (int i = 0; i < g_fake.dns_calls; i++) {
        if (!s_closed[i]) {
            srp_done(i, g_fake.dns[i].server == REMOTE_A ? a_error : MESH_SCAN_OT_ERROR_NONE);
        }
    }
}

static int in_flight(void)
{
    int n = 0;

    for (int i = 0; i < g_fake.dns_calls; i++) {
        n += !s_closed[i];
    }
    return n;
}

/** Queries to `server` issued so far, and of those, still open. */
static int calls_to(uint16_t server)
{
    int n = 0;

    for (int i = 0; i < g_fake.dns_calls; i++) {
        n += g_fake.dns[i].server == server;
    }
    return n;
}

static int in_flight_of(uint16_t server)
{
    int n = 0;

    for (int i = 0; i < g_fake.dns_calls; i++) {
        n += !s_closed[i] && g_fake.dns[i].server == server;
    }
    return n;
}

/** The first open query to `server`, or -1. */
static int open_call_of(uint16_t server)
{
    for (int i = 0; i < g_fake.dns_calls; i++) {
        if (!s_closed[i] && g_fake.dns[i].server == server) {
            return i;
        }
    }
    return -1;
}

/** An undescribed browse answer: instance only, no host, no address. */
static mesh_scan_srp_record_t bare_rec(const char *instance)
{
    mesh_scan_srp_record_t rec = { 0 };

    strcpy(rec.instance, instance);
    return rec;
}

static const mesh_scan_srp_host_t *srp_host(const mesh_scan_result_t *res, const char *name)
{
    for (uint8_t i = 0; i < res->n_srp; i++) {
        if (!strcmp(res->srp[i].name, name)) {
            return &res->srp[i];
        }
    }
    return NULL;
}

static const mesh_scan_srp_server_t *srp_server_of(const mesh_scan_result_t *res, uint16_t rloc16)
{
    for (uint8_t i = 0; i < res->n_srp_servers; i++) {
        if (res->srp_servers[i].rloc16 == rloc16) {
            return &res->srp_servers[i];
        }
    }
    TEST_FAIL_MESSAGE("no such server");
    return NULL;
}

/** Answer REMOTE_A's _matter._tcp browse with `rec`, end every query, end the mesh phases: the scan is done. */
static const mesh_scan_result_t *srp_finish_with(const mesh_scan_srp_record_t *rec)
{
    srp_answer(browse_of(REMOTE_A, "_matter._tcp"), rec);
    srp_drain(MESH_SCAN_OT_ERROR_NONE);
    mesh_over();
    return done_result();
}

// start

TEST_CASE("Two servers in the network data, one is this board: browses only the remote one, 7 jobs, at most 4 in "
          "flight", "[mesh_scan]")
{
    static const char *const types[SRP_TYPES] = { "_matter._tcp", "_matterc._udp", "_esphomelib._tcp", "_hap._udp",
                                                  "_http._tcp", "_meshcop._udp", "_trel._udp"
                                                };

    srp_start(1);
    TEST_ASSERT_EQUAL(MESH_SCAN_SRP_QUERIES, g_fake.dns_calls);
    for (int i = 0; i < SRP_TYPES; i++) {
        TEST_ASSERT_LESS_OR_EQUAL(MESH_SCAN_SRP_QUERIES, in_flight());
        srp_done(i, MESH_SCAN_OT_ERROR_NONE);
    }
    TEST_ASSERT_EQUAL(SRP_TYPES, g_fake.dns_calls);
    for (int i = 0; i < SRP_TYPES; i++) {
        TEST_ASSERT_FALSE(g_fake.dns[i].resolve);
        TEST_ASSERT_EQUAL_HEX16(REMOTE_A, g_fake.dns[i].server);
        TEST_ASSERT_EQUAL_STRING(types[i], g_fake.dns[i].type);
        TEST_ASSERT_EQUAL(g_fake.armed_gen, g_fake.dns[i].gen);
    }
}

TEST_CASE("Two servers in the network data, one is this board: arms the SRP deadline with 15000 ms and the current "
          "generation", "[mesh_scan]")
{
    srp_start(1);
    TEST_ASSERT_EQUAL(1, g_fake.srp_deadline_calls);
    TEST_ASSERT_EQUAL(MESH_SCAN_SRP_BUDGET_MS, g_fake.srp_deadline_ms);
    TEST_ASSERT_EQUAL(15000, MESH_SCAN_SRP_BUDGET_MS);
    TEST_ASSERT_EQUAL(g_fake.armed_gen, g_fake.srp_deadline_gen);
}

TEST_CASE("Two servers in the network data, one is this board: discover still starts at once; returns ESP_OK",
          "[mesh_scan]")
{
    srp_setup(1);
    TEST_ASSERT_EQUAL(ESP_OK, mesh_scan_core_start());
    TEST_ASSERT_EQUAL(1, g_fake.discover_calls);
    TEST_ASSERT_EQUAL(MESH_SCAN_RUNNING, view().state);
}

TEST_CASE("No remote server: no DNS query, SRP step counts as done", "[mesh_scan]")
{
    srp_start(0);
    TEST_ASSERT_EQUAL(1, g_fake.srp_servers_calls);
    TEST_ASSERT_EQUAL(0, g_fake.dns_calls);
    TEST_ASSERT_EQUAL(0, g_fake.srp_deadline_calls);
    TEST_ASSERT_EQUAL(2, g_fake.allocs); // no stage
    mesh_over();
    const mesh_scan_result_t *res = done_result();
    TEST_ASSERT_EQUAL(1, res->n_srp_servers);
    TEST_ASSERT_TRUE(res->srp_servers[0].this_board);
    TEST_ASSERT_EQUAL(MESH_SCAN_SRP_ANSWERED, res->srp_servers[0].state);
}

TEST_CASE("ops.dns_browse fails for one job: that job counts as done, the rest go on", "[mesh_scan]")
{
    srp_setup(1);
    g_fake.dns_rets[1] = MESH_SCAN_OT_ERROR_NO_BUFS;
    TEST_ASSERT_EQUAL(ESP_OK, mesh_scan_core_start());
    s_closed[1] = true; // never started
    TEST_ASSERT_EQUAL(MESH_SCAN_SRP_QUERIES + 1, g_fake.dns_calls);
    TEST_ASSERT_EQUAL(MESH_SCAN_SRP_QUERIES, in_flight());
    mesh_over();
    srp_drain(MESH_SCAN_OT_ERROR_NONE);
    TEST_ASSERT_EQUAL(SRP_TYPES, g_fake.dns_calls);
    TEST_ASSERT_EQUAL(MESH_SCAN_SRP_ANSWERED, srp_server_of(done_result(), REMOTE_A)->state);
}

TEST_CASE("When the SRP stage cannot be allocated: no DNS query, remote servers no_answer, the scan finishes",
          "[mesh_scan]")
{
    srp_setup(1);
    g_fake.fail_alloc_at = 3; // result, pool, stage
    TEST_ASSERT_EQUAL(ESP_OK, mesh_scan_core_start());
    TEST_ASSERT_EQUAL(0, g_fake.dns_calls);
    mesh_over();
    TEST_ASSERT_EQUAL(MESH_SCAN_SRP_NO_ANSWER, srp_server_of(done_result(), REMOTE_A)->state);
}

// on_srp_record

TEST_CASE("Browse answer described (SRV, TXT, address) and address kept: staged as a remote host with source = server "
          "rloc16, ttl_s, port, TXT in the pool", "[mesh_scan]")
{
    srp_start(1);
    const mesh_scan_srp_record_t rec = srp_rec("Plug 1", "plug-1", ADDR_OMR);
    const mesh_scan_result_t *res = srp_finish_with(&rec);
    const mesh_scan_srp_host_t *h = srp_host(res, "plug-1");

    TEST_ASSERT_NOT_NULL(h);
    TEST_ASSERT_FALSE(h->local);
    TEST_ASSERT_EQUAL_HEX16(REMOTE_A, h->source);
    TEST_ASSERT_EQUAL(120, h->ttl_s);
    TEST_ASSERT_EQUAL(1, h->n_addrs);
    TEST_ASSERT_EQUAL_HEX8(ADDR_OMR, h->addrs[0][0]);
    TEST_ASSERT_EQUAL(1, h->n_services);
    TEST_ASSERT_EQUAL_STRING("Plug 1", h->services[0].instance);
    TEST_ASSERT_EQUAL_STRING("_matter._tcp", h->services[0].type);
    TEST_ASSERT_EQUAL(5540, h->services[0].port);
    TEST_ASSERT_EQUAL_HEX16(REMOTE_A, h->services[0].source);
    TEST_ASSERT_TRUE(h->services[0].has_txt);
    TEST_ASSERT_FALSE(h->services[0].txt_truncated);
    TEST_ASSERT_EQUAL(sizeof(TXT) - 1, h->services[0].txt_len);
    TEST_ASSERT_EQUAL_MEMORY(TXT, res->pool + h->services[0].txt_off, sizeof(TXT) - 1);
}

TEST_CASE("Browse answer not described: a resolve is queued for that instance and type", "[mesh_scan]")
{
    srp_start(1);
    mesh_scan_srp_record_t rec = srp_rec("Plug 1", "", ADDR_OMR);
    rec.described = false;
    rec.n_addrs = 0;
    srp_answer(browse_of(REMOTE_A, "_hap._udp"), &rec);
    srp_drain(MESH_SCAN_OT_ERROR_NONE);

    const int r = find_call(REMOTE_A, "_hap._udp", true);
    TEST_ASSERT_GREATER_OR_EQUAL(0, r);
    TEST_ASSERT_EQUAL_STRING("Plug 1", g_fake.dns[r].instance);
    TEST_ASSERT_EQUAL(SRP_TYPES + 1, g_fake.dns_calls);
}

TEST_CASE("Browse answer not described: the resolve's answer is staged", "[mesh_scan]")
{
    srp_start(1);
    mesh_scan_srp_record_t rec = srp_rec("Plug 1", "plug-1", ADDR_OMR);
    rec.described = false;
    srp_answer(browse_of(REMOTE_A, "_hap._udp"), &rec);
    TEST_ASSERT_LESS_THAN(0, find_call(REMOTE_A, "_hap._udp", true)); // queued behind the browses
    for (int i = 0; find_call(REMOTE_A, "_hap._udp", true) < 0 && i < g_fake.dns_calls; i++) {
        if (!s_closed[i]) {
            srp_done(i, MESH_SCAN_OT_ERROR_NONE);
        }
    }
    const int r = find_call(REMOTE_A, "_hap._udp", true);
    rec.described = true;
    srp_answer(r, &rec);
    srp_drain(MESH_SCAN_OT_ERROR_NONE);
    mesh_over();
    const mesh_scan_srp_host_t *h = srp_host(done_result(), "plug-1");
    TEST_ASSERT_NOT_NULL(h);
    TEST_ASSERT_EQUAL(1, h->n_services); // the browse data itself was not staged
    TEST_ASSERT_EQUAL_STRING("_hap._udp", h->services[0].type);
}

TEST_CASE("Browse answer not described, its addresses all outside the OMR and mesh-local prefixes: no resolve, "
          "dropped", "[mesh_scan]")
{
    srp_start(1);
    mesh_scan_srp_record_t rec = srp_rec("Plug 1", "plug-1", ADDR_LAN); // LAN host from a discovery proxy
    rec.described = false;
    rec.n_addrs = 2;
    memset(rec.addrs[1], 0x30, 8);
    const mesh_scan_result_t *res = srp_finish_with(&rec);

    TEST_ASSERT_LESS_THAN(0, find_call(REMOTE_A, "_matter._tcp", true));
    TEST_ASSERT_EQUAL(SRP_TYPES, g_fake.dns_calls);
    TEST_ASSERT_NULL(srp_host(res, "plug-1"));
}

TEST_CASE("Browse answer not described, one address under the OMR prefix among LAN ones: a resolve is queued",
          "[mesh_scan]")
{
    srp_start(1);
    mesh_scan_srp_record_t rec = srp_rec("Plug 1", "plug-1", ADDR_LAN);
    rec.described = false;
    rec.n_addrs = 2;
    memset(rec.addrs[1], ADDR_OMR, 8);
    srp_answer(browse_of(REMOTE_A, "_matter._tcp"), &rec);
    srp_drain(MESH_SCAN_OT_ERROR_NONE);
    TEST_ASSERT_GREATER_OR_EQUAL(0, find_call(REMOTE_A, "_matter._tcp", true));
}

TEST_CASE("Browse answer not described, a _meshcop._udp record with a LAN address: a resolve is still queued (judged "
          "by TXT)", "[mesh_scan]")
{
    srp_start(1);
    mesh_scan_srp_record_t rec = srp_rec("HA OTBR", "ha-otbr", ADDR_LAN);
    rec.described = false;
    srp_answer(browse_of(REMOTE_A, "_meshcop._udp"), &rec);
    srp_drain(MESH_SCAN_OT_ERROR_NONE);
    TEST_ASSERT_GREATER_OR_EQUAL(0, find_call(REMOTE_A, "_meshcop._udp", true));
}

/** The call index of the resolve of `instance`, or -1. */
static int resolve_of(const char *instance)
{
    for (int i = 0; i < g_fake.dns_calls; i++) {
        if (g_fake.dns[i].resolve && !strcmp(g_fake.dns[i].instance, instance)) {
            return i;
        }
    }
    return -1;
}

TEST_CASE("Resolves queued for several types: _esphomelib._tcp and _hap._udp ones are issued before the others",
          "[mesh_scan]")
{
    srp_start(1);
    const mesh_scan_srp_record_t m1 = bare_rec("M1"), m2 = bare_rec("M2"), h = bare_rec("H"), e = bare_rec("E");
    const int bm = find_call(REMOTE_A, "_matter._tcp", false);
    srp_answer(bm, &m1);
    srp_answer(bm, &m2);
    srp_answer(find_call(REMOTE_A, "_hap._udp", false), &h);
    srp_answer(find_call(REMOTE_A, "_esphomelib._tcp", false), &e);
    srp_drain(MESH_SCAN_OT_ERROR_NONE);

    TEST_ASSERT_GREATER_OR_EQUAL(0, resolve_of("M2"));
    TEST_ASSERT_LESS_THAN(resolve_of("M1"), resolve_of("H"));
    TEST_ASSERT_LESS_THAN(resolve_of("M1"), resolve_of("E"));
    TEST_ASSERT_LESS_THAN(resolve_of("M2"), resolve_of("M1")); // in queue order within a rank
    TEST_ASSERT_LESS_THAN(resolve_of("E"), resolve_of("H"));
}

TEST_CASE("49th resolve wanted (a name-bearing type): not queued; the browse data is kept if it passes the filter, else "
          "dropped", "[mesh_scan]")
{
    char name[16];

    srp_start(1);
    const int b = browse_of(REMOTE_A, "_hap._udp");
    for (int i = 0; i < MESH_SCAN_SRP_RESOLVES + 2; i++) {
        snprintf(name, sizeof(name), "dev-%d", i);
        // the 49th passes the filter, the 50th does not
        mesh_scan_srp_record_t rec = srp_rec(name, name, i == MESH_SCAN_SRP_RESOLVES + 1 ? ADDR_LAN : ADDR_OMR);
        rec.described = false;
        srp_answer(b, &rec);
    }
    srp_drain(MESH_SCAN_OT_ERROR_NONE);
    mesh_over();
    int resolves = 0;
    for (int i = 0; i < g_fake.dns_calls; i++) {
        resolves += g_fake.dns[i].resolve;
    }
    TEST_ASSERT_EQUAL(MESH_SCAN_SRP_RESOLVES, resolves);
    TEST_ASSERT_EQUAL(48, MESH_SCAN_SRP_RESOLVES);
    const mesh_scan_result_t *res = done_result();
    TEST_ASSERT_NOT_NULL(srp_host(res, "dev-48"));
    TEST_ASSERT_NULL(srp_host(res, "dev-49"));
    TEST_ASSERT_NULL(srp_host(res, "dev-0")); // resolved, and its resolve gave no record
}

TEST_CASE("48 undescribed Matter instances answered first: 32 of them resolved, an undescribed ESPHome instance still "
          "gets a resolve", "[mesh_scan]")
{
    char name[16];

    srp_start(1);
    const int bm = find_call(REMOTE_A, "_matter._tcp", false);
    for (int i = 0; i < MESH_SCAN_SRP_RESOLVES; i++) {
        snprintf(name, sizeof(name), "m-%d", i);
        const mesh_scan_srp_record_t rec = bare_rec(name);
        srp_answer(bm, &rec);
    }
    const mesh_scan_srp_record_t e = bare_rec("E");
    srp_answer(find_call(REMOTE_A, "_esphomelib._tcp", false), &e);
    srp_drain(MESH_SCAN_OT_ERROR_NONE);

    int matter = 0;
    for (int i = 0; i < g_fake.dns_calls; i++) {
        matter += g_fake.dns[i].resolve && !strcmp(g_fake.dns[i].type, "_matter._tcp");
    }
    TEST_ASSERT_EQUAL(16, MESH_SCAN_SRP_NAME_RESOLVES);
    TEST_ASSERT_EQUAL(MESH_SCAN_SRP_RESOLVES - MESH_SCAN_SRP_NAME_RESOLVES, matter);
    TEST_ASSERT_GREATER_OR_EQUAL(0, resolve_of("E"));
}

TEST_CASE("Address only on the LAN: dropped", "[mesh_scan]")
{
    srp_start(1);
    const mesh_scan_srp_record_t rec = srp_rec("Plug 1", "plug-1", ADDR_LAN);
    const mesh_scan_result_t *res = srp_finish_with(&rec);

    TEST_ASSERT_NULL(srp_host(res, "plug-1"));
    TEST_ASSERT_EQUAL(1, res->n_srp);
    TEST_ASSERT_EQUAL(0, srp_server_of(res, REMOTE_A)->hosts);
}

TEST_CASE("Address under the mesh-local prefix: kept", "[mesh_scan]")
{
    srp_start(1);
    const mesh_scan_srp_record_t rec = srp_rec("Plug 1", "plug-1", ADDR_ML);
    TEST_ASSERT_NOT_NULL(srp_host(srp_finish_with(&rec), "plug-1"));
}

/** A meshcop TXT with `xp` = `xp` and `xa` = `xa`. */
static mesh_scan_srp_record_t meshcop_rec(const uint8_t xp[8], const uint8_t xa[8])
{
    mesh_scan_srp_record_t rec = srp_rec("HA OTBR", "ha-otbr", ADDR_LAN);

    s_txt[0] = 11;
    memcpy(s_txt + 1, "xp=", 3);
    memcpy(s_txt + 4, xp, 8);
    s_txt[12] = 11;
    memcpy(s_txt + 13, "xa=", 3);
    memcpy(s_txt + 16, xa, 8);
    rec.txt = s_txt;
    rec.txt_len = 24;
    return rec;
}

TEST_CASE("meshcop record with this network's xp from a LAN address: kept", "[mesh_scan]")
{
    static const uint8_t other_ext[8] = { 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22 };
    static const uint8_t other_pan[8] = { 9, 9, 9, 9, 9, 9, 9, 9 };

    srp_start(1);
    mesh_scan_srp_record_t ours = meshcop_rec(EXT_PAN_ID, other_ext);
    srp_answer(browse_of(REMOTE_A, "_meshcop._udp"), &ours);
    mesh_scan_srp_record_t foreign = meshcop_rec(other_pan, other_ext);
    strcpy(foreign.host, "neighbour-otbr");
    srp_answer(browse_of(REMOTE_A, "_meshcop._udp"), &foreign);
    mesh_scan_srp_record_t self = meshcop_rec(EXT_PAN_ID, OWN_EXT);
    strcpy(self.host, "this-otbr");
    srp_answer(browse_of(REMOTE_A, "_meshcop._udp"), &self);
    srp_drain(MESH_SCAN_OT_ERROR_NONE);
    mesh_over();

    const mesh_scan_result_t *res = done_result();
    const mesh_scan_srp_host_t *h = srp_host(res, "ha-otbr");
    TEST_ASSERT_NOT_NULL(h);
    TEST_ASSERT_EQUAL_STRING("_meshcop._udp", h->services[0].type);
    TEST_ASSERT_NULL(srp_host(res, "neighbour-otbr"));
    TEST_ASSERT_NULL(srp_host(res, "this-otbr"));
}

TEST_CASE("Same host name already staged from another server: one host, services merged, host fields of the first "
          "kept", "[mesh_scan]")
{
    srp_start(2);
    mesh_scan_srp_record_t a = srp_rec("Plug 1", "plug-1", ADDR_OMR);
    srp_answer(browse_of(REMOTE_A, "_matter._tcp"), &a);
    mesh_scan_srp_record_t b = srp_rec("Plug 1", "PLUG-1", ADDR_ML); // same service again
    b.ttl_s = 999;
    srp_answer(browse_of(REMOTE_B, "_matter._tcp"), &b);
    mesh_scan_srp_record_t c = srp_rec("plug-esp", "Plug-1", ADDR_ML);
    srp_answer(browse_of(REMOTE_B, "_esphomelib._tcp"), &c);
    srp_drain(MESH_SCAN_OT_ERROR_NONE);
    mesh_over();

    const mesh_scan_result_t *res = done_result();
    TEST_ASSERT_EQUAL(2, res->n_srp); // lamp and plug-1
    const mesh_scan_srp_host_t *h = srp_host(res, "plug-1");
    TEST_ASSERT_NOT_NULL(h);
    TEST_ASSERT_EQUAL_HEX16(REMOTE_A, h->source);
    TEST_ASSERT_EQUAL(120, h->ttl_s);
    TEST_ASSERT_EQUAL_HEX8(ADDR_OMR, h->addrs[0][0]);
    TEST_ASSERT_EQUAL(2, h->n_services);
    TEST_ASSERT_EQUAL_STRING("_matter._tcp", h->services[0].type);
    TEST_ASSERT_EQUAL_HEX16(REMOTE_A, h->services[0].source);
    TEST_ASSERT_EQUAL_STRING("_esphomelib._tcp", h->services[1].type);
    TEST_ASSERT_EQUAL_HEX16(REMOTE_B, h->services[1].source);
}

TEST_CASE("TXT of 700 bytes: stored cut to whole strings within 512, txt_truncated true", "[mesh_scan]")
{
    for (int i = 0; i < 10; i++) { // ten strings of 1 + 69 bytes
        s_txt[i * 70] = 69;
        memset(s_txt + i * 70 + 1, 'a' + i, 69);
    }
    srp_start(1);
    mesh_scan_srp_record_t rec = srp_rec("Plug 1", "plug-1", ADDR_OMR);
    rec.txt = s_txt;
    rec.txt_len = 700;
    const mesh_scan_result_t *res = srp_finish_with(&rec);
    const mesh_scan_srp_host_t *h = srp_host(res, "plug-1");

    TEST_ASSERT_TRUE(h->services[0].has_txt);
    TEST_ASSERT_TRUE(h->services[0].txt_truncated);
    TEST_ASSERT_EQUAL(490, h->services[0].txt_len);
    TEST_ASSERT_EQUAL_MEMORY(s_txt, res->pool + h->services[0].txt_off, 490);
}

TEST_CASE("TXT cut by ot_ops: txt_truncated true", "[mesh_scan]")
{
    srp_start(1);
    mesh_scan_srp_record_t rec = srp_rec("Plug 1", "plug-1", ADDR_OMR);
    rec.txt_truncated = true;
    const mesh_scan_srp_host_t *h = srp_host(srp_finish_with(&rec), "plug-1");

    TEST_ASSERT_TRUE(h->services[0].txt_truncated);
    TEST_ASSERT_EQUAL(sizeof(TXT) - 1, h->services[0].txt_len);
}

TEST_CASE("Pool full: service kept without TXT, txt_truncated true", "[mesh_scan]")
{
    srp_start(1);
    g_fake.snapshot_pool_used = MESH_SCAN_POOL_SIZE - 2; // this board's records used it up
    const mesh_scan_srp_record_t rec = srp_rec("Plug 1", "plug-1", ADDR_OMR);
    const mesh_scan_result_t *res = srp_finish_with(&rec);
    const mesh_scan_srp_host_t *h = srp_host(res, "plug-1");

    TEST_ASSERT_EQUAL(1, h->n_services);
    TEST_ASSERT_FALSE(h->services[0].has_txt);
    TEST_ASSERT_EQUAL(0, h->services[0].txt_len);
    TEST_ASSERT_TRUE(h->services[0].txt_truncated);
    TEST_ASSERT_EQUAL(MESH_SCAN_POOL_SIZE - 2, res->pool_used);
}

TEST_CASE("Stale generation or a job not in flight: ignored, nothing written", "[mesh_scan]")
{
    srp_start(1);
    const int b = browse_of(REMOTE_A, "_matter._tcp");
    const mesh_scan_srp_record_t rec = srp_rec("Plug 1", "plug-1", ADDR_OMR);
    const int calls = g_fake.dns_calls;

    mesh_scan_on_srp_record(g_fake.dns[b].gen - 1, g_fake.dns[b].job, &rec); // older generation
    mesh_scan_on_srp_record(g_fake.dns[b].gen, 6, &rec);                    // queued, not issued yet
    mesh_scan_on_srp_record(g_fake.dns[b].gen, 500, &rec);                  // no such job
    mesh_scan_on_srp_query_done(g_fake.dns[b].gen - 1, g_fake.dns[b].job, MESH_SCAN_OT_ERROR_NONE);
    mesh_scan_on_srp_query_done(g_fake.dns[b].gen, 6, MESH_SCAN_OT_ERROR_NONE);
    TEST_ASSERT_EQUAL(calls, g_fake.dns_calls); // no slot freed
    srp_done(b, MESH_SCAN_OT_ERROR_NONE);
    srp_answer(b, &rec);                         // after its done
    mesh_scan_on_srp_query_done(g_fake.dns[b].gen, g_fake.dns[b].job, MESH_SCAN_OT_ERROR_NONE); // twice
    TEST_ASSERT_EQUAL(calls + 1, g_fake.dns_calls);
    srp_drain(MESH_SCAN_OT_ERROR_NONE);
    mesh_over();
    const mesh_scan_result_t *res = done_result();
    TEST_ASSERT_NULL(srp_host(res, "plug-1"));
    TEST_ASSERT_EQUAL(0, srp_server_of(res, REMOTE_A)->hosts);
}

TEST_CASE("Late DNS callbacks of an older scan (Review focus 4): dropped, the newer scan and its result untouched",
          "[mesh_scan]")
{
    srp_start(1);
    const fake_dns_call_t old = g_fake.dns[0];
    const int old_calls = g_fake.dns_calls;
    mesh_scan_core_cancel(); // the old stage and result are released
    TEST_ASSERT_EQUAL(g_fake.allocs, g_fake.releases);

    TEST_ASSERT_EQUAL(ESP_OK, mesh_scan_core_start());
    const int calls = g_fake.dns_calls;
    const mesh_scan_srp_record_t rec = srp_rec("Plug 1", "plug-1", ADDR_OMR);
    mesh_scan_on_srp_record(old.gen, old.job, &rec); // job 0 is in flight in the new scan too
    mesh_scan_on_srp_query_done(old.gen, old.job, MESH_SCAN_OT_ERROR_NONE);
    mesh_scan_on_srp_deadline(old.gen);
    TEST_ASSERT_EQUAL(calls, g_fake.dns_calls);
    TEST_ASSERT_EQUAL(MESH_SCAN_RUNNING, view().state);

    memset(s_closed, 0, sizeof(s_closed));
    for (int i = 0; i < old_calls; i++) {
        s_closed[i] = true; // the old scan's
    }
    srp_drain(MESH_SCAN_OT_ERROR_NONE);
    mesh_over();
    TEST_ASSERT_NULL(srp_host(done_result(), "plug-1"));
}

// on_srp_query_done

TEST_CASE("On a query done with NONE: server state answered; next queued job issued", "[mesh_scan]")
{
    srp_start(1);
    srp_done(0, MESH_SCAN_OT_ERROR_NONE);
    TEST_ASSERT_EQUAL(MESH_SCAN_SRP_QUERIES + 1, g_fake.dns_calls);
    TEST_ASSERT_EQUAL_STRING("_http._tcp", g_fake.dns[MESH_SCAN_SRP_QUERIES].type);
    for (int i = 1; i < g_fake.dns_calls; i++) {
        if (!s_closed[i]) {
            srp_done(i, MESH_SCAN_OT_ERROR_RESPONSE_TIMEOUT); // one answer is enough
        }
    }
    mesh_over();
    TEST_ASSERT_EQUAL(MESH_SCAN_SRP_ANSWERED, srp_server_of(done_result(), REMOTE_A)->state);
}

TEST_CASE("On a query done with NOT_FOUND (empty answer): server state answered", "[mesh_scan]")
{
    srp_start(1);
    srp_drain(MESH_SCAN_OT_ERROR_NOT_FOUND);
    mesh_over();
    TEST_ASSERT_EQUAL(MESH_SCAN_SRP_ANSWERED, srp_server_of(done_result(), REMOTE_A)->state);
}

TEST_CASE("Every query of a server timed out (Review focus 1): server state no_answer at the end, the other server's "
          "records intact", "[mesh_scan]")
{
    srp_start(2);
    const mesh_scan_srp_record_t rec = srp_rec("Plug 1", "plug-1", ADDR_OMR);
    srp_answer(browse_of(REMOTE_B, "_matter._tcp"), &rec);
    mesh_over();
    srp_drain(MESH_SCAN_OT_ERROR_RESPONSE_TIMEOUT);

    const mesh_scan_result_t *res = done_result();
    TEST_ASSERT_EQUAL(MESH_SCAN_SRP_NO_ANSWER, srp_server_of(res, REMOTE_A)->state);
    TEST_ASSERT_EQUAL(MESH_SCAN_SRP_ANSWERED, srp_server_of(res, REMOTE_B)->state);
    TEST_ASSERT_EQUAL(1, srp_server_of(res, REMOTE_B)->hosts);
    TEST_ASSERT_EQUAL_HEX16(REMOTE_B, srp_host(res, "plug-1")->source);
    TEST_ASSERT_FALSE(res->partial);
}

TEST_CASE("Every query of a server timed out (Review focus 1): after its first timeout no further job goes to that "
          "server; the other server's browses and resolves are issued", "[mesh_scan]")
{
    srp_start(2);
    const int b = find_call(REMOTE_B, "_matter._tcp", false);
    const mesh_scan_srp_record_t rec = bare_rec("Plug 1");
    srp_answer(b, &rec);                                        // queues a resolve on REMOTE_B
    srp_done(open_call_of(REMOTE_A), MESH_SCAN_OT_ERROR_RESPONSE_TIMEOUT); // REMOTE_A never answered: given up
    const int to_a = calls_to(REMOTE_A);
    while (open_call_of(REMOTE_B) >= 0) {
        srp_done(open_call_of(REMOTE_B), MESH_SCAN_OT_ERROR_NONE);
    }
    TEST_ASSERT_EQUAL(to_a, calls_to(REMOTE_A));
    TEST_ASSERT_EQUAL(SRP_TYPES + 1, calls_to(REMOTE_B));
    TEST_ASSERT_GREATER_OR_EQUAL(0, find_call(REMOTE_B, "_matter._tcp", true));
    srp_drain(MESH_SCAN_OT_ERROR_RESPONSE_TIMEOUT);
    mesh_over();
    TEST_ASSERT_EQUAL(to_a, calls_to(REMOTE_A));
    TEST_ASSERT_EQUAL(MESH_SCAN_SRP_NO_ANSWER, srp_server_of(done_result(), REMOTE_A)->state);
}

TEST_CASE("Two remote servers, one silent: no server has more than 2 queries in flight", "[mesh_scan]")
{
    srp_start(2);
    while (open_call_of(REMOTE_B) >= 0) {
        TEST_ASSERT_LESS_OR_EQUAL(2, in_flight_of(REMOTE_A));
        TEST_ASSERT_LESS_OR_EQUAL(2, in_flight_of(REMOTE_B));
        srp_done(open_call_of(REMOTE_B), MESH_SCAN_OT_ERROR_NONE);
    }
    TEST_ASSERT_EQUAL(2, in_flight_of(REMOTE_A));
    TEST_ASSERT_EQUAL(SRP_TYPES, calls_to(REMOTE_B));
}

TEST_CASE("Two remote servers, one given up: the live one may use all 4 slots", "[mesh_scan]")
{
    srp_start(2);
    srp_done(open_call_of(REMOTE_A), MESH_SCAN_OT_ERROR_RESPONSE_TIMEOUT); // REMOTE_A given up
    TEST_ASSERT_EQUAL(3, in_flight_of(REMOTE_B));                          // REMOTE_A still holds one slot
    srp_done(open_call_of(REMOTE_A), MESH_SCAN_OT_ERROR_RESPONSE_TIMEOUT);
    TEST_ASSERT_EQUAL(MESH_SCAN_SRP_QUERIES, in_flight_of(REMOTE_B));
}

TEST_CASE("One remote server: up to 4 queries in flight to it", "[mesh_scan]")
{
    srp_start(1);
    TEST_ASSERT_EQUAL(MESH_SCAN_SRP_QUERIES, in_flight_of(REMOTE_A));
}

TEST_CASE("Two remote servers: their browses alternate, so a silent one cannot hold back the other", "[mesh_scan]")
{
    srp_start(2);
    TEST_ASSERT_EQUAL_HEX16(REMOTE_A, g_fake.dns[0].server);
    TEST_ASSERT_EQUAL_HEX16(REMOTE_B, g_fake.dns[1].server);
    TEST_ASSERT_EQUAL_HEX16(REMOTE_A, g_fake.dns[2].server);
    TEST_ASSERT_EQUAL_HEX16(REMOTE_B, g_fake.dns[3].server);
}

TEST_CASE("Last job of the step done while child phases still run: scan stays running", "[mesh_scan]")
{
    srp_start(1);
    srp_drain(MESH_SCAN_OT_ERROR_NONE);
    TEST_ASSERT_EQUAL(MESH_SCAN_RUNNING, view().state);
    TEST_ASSERT_EQUAL(0, g_fake.snapshot_calls);
    mesh_over();
    done_result();
}

TEST_CASE("Last job done after the mesh phases are over: scan finishes (state done)", "[mesh_scan]")
{
    srp_start(1);
    mesh_over();
    TEST_ASSERT_EQUAL(MESH_SCAN_RUNNING, view().state);
    srp_drain(MESH_SCAN_OT_ERROR_NONE);
    done_result();
    TEST_ASSERT_EQUAL(1, g_fake.snapshot_calls);
}

// on_srp_deadline

TEST_CASE("On the SRP deadline with queries in flight: their servers cut_short; queued jobs dropped; later callbacks "
          "ignored", "[mesh_scan]")
{
    srp_start(1);
    mesh_over();
    srp_done(0, MESH_SCAN_OT_ERROR_NONE); // answered once
    const int calls = g_fake.dns_calls;
    mesh_scan_on_srp_deadline(g_fake.srp_deadline_gen);

    const mesh_scan_result_t *res = done_result();
    TEST_ASSERT_EQUAL(MESH_SCAN_SRP_CUT_SHORT, srp_server_of(res, REMOTE_A)->state);
    TEST_ASSERT_EQUAL(calls, g_fake.dns_calls);
    const mesh_scan_srp_record_t rec = srp_rec("Plug 1", "plug-1", ADDR_OMR);
    srp_answer(1, &rec); // in flight at the deadline
    srp_done(1, MESH_SCAN_OT_ERROR_NONE);
    TEST_ASSERT_EQUAL(calls, g_fake.dns_calls);
    TEST_ASSERT_NULL(srp_host(done_result(), "plug-1"));
}

TEST_CASE("On the SRP deadline, a server that never answered with queries still in flight or queued: cut_short",
          "[mesh_scan]")
{
    srp_start(1);
    mesh_over();
    mesh_scan_on_srp_deadline(g_fake.srp_deadline_gen);
    TEST_ASSERT_EQUAL(MESH_SCAN_SRP_CUT_SHORT, srp_server_of(done_result(), REMOTE_A)->state);
}

TEST_CASE("On the SRP deadline, a server given up after a timeout: no_answer, though queries were still in flight",
          "[mesh_scan]")
{
    srp_start(1);
    mesh_over();
    srp_done(0, MESH_SCAN_OT_ERROR_RESPONSE_TIMEOUT);
    TEST_ASSERT_EQUAL(MESH_SCAN_SRP_QUERIES - 1, in_flight());
    TEST_ASSERT_EQUAL(MESH_SCAN_SRP_QUERIES, g_fake.dns_calls); // its queued browses dropped
    mesh_scan_on_srp_deadline(g_fake.srp_deadline_gen);
    TEST_ASSERT_EQUAL(MESH_SCAN_SRP_NO_ANSWER, srp_server_of(done_result(), REMOTE_A)->state);
}

TEST_CASE("A server given up after a timeout that answers a query still in flight: cut_short, its records kept",
          "[mesh_scan]")
{
    srp_start(1);
    mesh_over();
    srp_done(0, MESH_SCAN_OT_ERROR_RESPONSE_TIMEOUT); // given up, its queued browses dropped
    const mesh_scan_srp_record_t rec = srp_rec("Plug 1", "plug-1", ADDR_OMR);
    srp_answer(1, &rec);
    srp_drain(MESH_SCAN_OT_ERROR_NONE);
    const mesh_scan_result_t *res = done_result();
    TEST_ASSERT_EQUAL(MESH_SCAN_SRP_QUERIES, g_fake.dns_calls);
    TEST_ASSERT_EQUAL(MESH_SCAN_SRP_CUT_SHORT, srp_server_of(res, REMOTE_A)->state);
    TEST_ASSERT_NOT_NULL(srp_host(res, "plug-1"));
}

TEST_CASE("Every query of a server failed to start: no_answer", "[mesh_scan]")
{
    srp_setup(1);
    for (int i = 0; i < SRP_TYPES; i++) {
        g_fake.dns_rets[i] = MESH_SCAN_OT_ERROR_NO_BUFS;
    }
    TEST_ASSERT_EQUAL(ESP_OK, mesh_scan_core_start());
    mesh_over();
    TEST_ASSERT_EQUAL(MESH_SCAN_SRP_NO_ANSWER, srp_server_of(done_result(), REMOTE_A)->state);
}

TEST_CASE("On the SRP deadline with the mesh phases still running: scan keeps running until they end",
          "[mesh_scan]")
{
    srp_start(1);
    const mesh_scan_srp_record_t rec = srp_rec("Plug 1", "plug-1", ADDR_OMR);
    srp_answer(0, &rec);
    mesh_scan_on_srp_deadline(g_fake.srp_deadline_gen);
    TEST_ASSERT_EQUAL(MESH_SCAN_RUNNING, view().state);
    mesh_over();
    TEST_ASSERT_NOT_NULL(srp_host(done_result(), "plug-1")); // what was staged before the deadline
}

// finish

TEST_CASE("Mesh phases over, SRP step not over: waits (state running)", "[mesh_scan]")
{
    srp_start(1);
    mesh_over();
    TEST_ASSERT_EQUAL(MESH_SCAN_RUNNING, view().state);
    TEST_ASSERT_EQUAL(0, g_fake.snapshot_calls);
    TEST_ASSERT_EQUAL(0, g_fake.disarm_calls);
}

TEST_CASE("Cap hit during the SRP step: finishes with what is staged; servers with queries in flight cut_short; "
          "partial unchanged by SRP", "[mesh_scan]")
{
    srp_start(1);
    mesh_over();
    const int b = browse_of(REMOTE_A, "_matter._tcp");
    const mesh_scan_srp_record_t rec = srp_rec("Plug 1", "plug-1", ADDR_OMR);
    srp_answer(b, &rec);
    srp_done(b, MESH_SCAN_OT_ERROR_NONE);
    mesh_scan_on_cap();

    const mesh_scan_result_t *res = done_result();
    TEST_ASSERT_NOT_NULL(srp_host(res, "plug-1"));
    TEST_ASSERT_EQUAL(MESH_SCAN_SRP_CUT_SHORT, srp_server_of(res, REMOTE_A)->state);
    TEST_ASSERT_FALSE(res->partial);
    TEST_ASSERT_EQUAL(1, g_fake.releases); // only the stage is gone

    // Review focus 4: a late answer after the cap
    const mesh_scan_srp_record_t late = srp_rec("Plug 2", "plug-2", ADDR_OMR);
    srp_answer(1, &late);
    srp_done(1, MESH_SCAN_OT_ERROR_NONE);
    mesh_scan_on_srp_deadline(g_fake.srp_deadline_gen);
    TEST_ASSERT_NULL(srp_host(done_result(), "plug-2"));
}

TEST_CASE("Cap hit during discover with the SRP step running: partial as before", "[mesh_scan]")
{
    srp_start(1);
    mesh_scan_on_router(router(3, 0));
    mesh_scan_on_cap();
    TEST_ASSERT_TRUE(done_result()->partial);
}

TEST_CASE("Local snapshot holds the same host as a remote one (Review focus 2): one host, source = this board, "
          "lease_s/key_lease_s set, ttl_s absent, services merged", "[mesh_scan]")
{
    srp_start(1);
    mesh_scan_srp_record_t same = srp_rec("lamp-svc", "LAMP", ADDR_OMR); // proxied copy of the local service
    srp_answer(browse_of(REMOTE_A, "_matter._tcp"), &same);
    mesh_scan_srp_record_t other = srp_rec("Lamp", "lamp", ADDR_OMR);
    srp_answer(browse_of(REMOTE_A, "_hap._udp"), &other);
    srp_drain(MESH_SCAN_OT_ERROR_NONE);
    mesh_over();

    const mesh_scan_result_t *res = done_result();
    TEST_ASSERT_EQUAL(1, res->n_srp);
    const mesh_scan_srp_host_t *h = &res->srp[0];
    TEST_ASSERT_EQUAL_STRING("lamp", h->name);
    TEST_ASSERT_TRUE(h->local);
    TEST_ASSERT_EQUAL_HEX16(THIS_BOARD, h->source);
    TEST_ASSERT_EQUAL(3600, h->lease_s);
    TEST_ASSERT_EQUAL(86400, h->key_lease_s);
    TEST_ASSERT_EQUAL(0, h->ttl_s);
    TEST_ASSERT_EQUAL(ADDR_ML, h->addrs[0][0]); // this board's addresses
    TEST_ASSERT_EQUAL(2, h->n_services);        // no duplicate row for lamp-svc
    TEST_ASSERT_EQUAL_STRING("lamp-svc", h->services[0].instance);
    TEST_ASSERT_EQUAL_HEX16(THIS_BOARD, h->services[0].source);
    TEST_ASSERT_EQUAL_STRING("Lamp", h->services[1].instance);
    TEST_ASSERT_EQUAL_STRING("_hap._udp", h->services[1].type);
    TEST_ASSERT_EQUAL_HEX16(REMOTE_A, h->services[1].source);
    TEST_ASSERT_EQUAL(1, srp_server_of(res, REMOTE_A)->hosts);
}

TEST_CASE("64 hosts already: further remote hosts dropped", "[mesh_scan]")
{
    srp_start(1);
    g_fake.snapshot_hosts = MESH_SCAN_MAX_SRP_HOSTS;
    const mesh_scan_srp_record_t rec = srp_rec("Plug 1", "plug-1", ADDR_OMR);
    const mesh_scan_result_t *res = srp_finish_with(&rec);

    TEST_ASSERT_EQUAL(MESH_SCAN_MAX_SRP_HOSTS, res->n_srp);
    TEST_ASSERT_NULL(srp_host(res, "plug-1"));
}

TEST_CASE("srp_servers: one entry per network data server, hosts = kept hosts from it", "[mesh_scan]")
{
    srp_start(2);
    mesh_scan_srp_record_t rec = srp_rec("Plug 1", "plug-1", ADDR_OMR);
    srp_answer(browse_of(REMOTE_A, "_matter._tcp"), &rec);
    rec = srp_rec("Plug 2", "plug-2", ADDR_OMR);
    srp_answer(browse_of(REMOTE_A, "_matter._tcp"), &rec);
    rec = srp_rec("Plug 1", "plug-1", ADDR_OMR);
    srp_answer(browse_of(REMOTE_B, "_matter._tcp"), &rec); // before de-duplication
    rec = srp_rec("Plug 1 HAP", "plug-1", ADDR_OMR);
    srp_answer(browse_of(REMOTE_A, "_hap._udp"), &rec); // same host again: counted once
    srp_drain(MESH_SCAN_OT_ERROR_NONE);
    mesh_over();

    const mesh_scan_result_t *res = done_result();
    TEST_ASSERT_EQUAL(3, res->n_srp_servers);
    const mesh_scan_srp_server_t *self = srp_server_of(res, THIS_BOARD);
    TEST_ASSERT_TRUE(self->this_board);
    TEST_ASSERT_EQUAL(MESH_SCAN_SRP_ANSWERED, self->state);
    TEST_ASSERT_EQUAL(1, self->hosts); // lamp
    TEST_ASSERT_EQUAL(53, self->port);
    TEST_ASSERT_EQUAL_HEX8(ADDR_ML, self->addr[0]);
    TEST_ASSERT_FALSE(srp_server_of(res, REMOTE_A)->this_board);
    TEST_ASSERT_EQUAL(2, srp_server_of(res, REMOTE_A)->hosts);
    TEST_ASSERT_EQUAL(1, srp_server_of(res, REMOTE_B)->hosts);
}

// SRP callbacks and the mode

TEST_CASE("When the mode is no longer BR_RUNNING: an SRP callback fails the scan with error stopped", "[mesh_scan]")
{
    const mesh_scan_srp_record_t rec = srp_rec("Plug 1", "plug-1", ADDR_OMR);

    for (int i = 0; i < 3; i++) {
        srp_start(1);
        g_fake.br_running = false;
        if (i == 0) {
            srp_answer(0, &rec);
        } else if (i == 1) {
            srp_done(0, MESH_SCAN_OT_ERROR_NONE);
        } else {
            mesh_scan_on_srp_deadline(g_fake.srp_deadline_gen);
        }
        assert_failed(MESH_SCAN_ERR_STOPPED);
        TEST_ASSERT_EQUAL(0, g_fake.cancel_calls);
        TEST_ASSERT_EQUAL(g_fake.allocs, g_fake.releases); // stage released with the result
    }
}

// result release

TEST_CASE("On a scan with an SRP step whose result is released: the pool and stage blocks go with it (allocs == "
          "releases)", "[mesh_scan]")
{
    srp_start(1);
    TEST_ASSERT_EQUAL(3, g_fake.allocs); // result, pool, stage
    const mesh_scan_srp_record_t rec = srp_rec("Plug 1", "plug-1", ADDR_OMR);
    srp_finish_with(&rec);
    TEST_ASSERT_EQUAL(1, g_fake.releases); // the stage, at finish
    mesh_scan_core_start();                // releases the done result and its pool
    mesh_scan_core_cancel();               // and this one with its stage
    TEST_ASSERT_EQUAL(g_fake.allocs, g_fake.releases);
}
