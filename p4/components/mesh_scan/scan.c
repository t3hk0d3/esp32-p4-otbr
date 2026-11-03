#include "scan.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "render.h"

static const char *TAG = "mesh_scan";

#define NO_ROUTER 0xff
#define FIRST_CHILDREN_CAPACITY 8

typedef enum {
    PHASE_DISCOVER,
    PHASE_CHILD_TABLE,
    PHASE_CHILD_ADDRS,
    PHASE_CHILD_DIAG, /**< asking rx-on children without addresses directly (spec 3.4) */
} phase_t;

/** A child address query in flight. */
typedef struct {
    mesh_scan_child_t *child; /**< NULL: the slot is free */
    int64_t due_ms;
} ask_slot_t;

/**
 * One scan's result. The scan holds one reference while it is current; each render in flight holds another. The
 * block and its children arrays are released with the last reference.
 */
typedef struct {
    uint32_t refs;
    mesh_scan_result_t result;
} block_t;

static struct {
    SemaphoreHandle_t lock;
    StaticSemaphore_t lock_storage;
    mesh_scan_ops_t ops;
    mesh_scan_state_t state;
    mesh_scan_error_t error;
    phase_t phase;
    block_t *block;   /**< current result: running and done only */
    uint8_t current;  /**< router being queried, NO_ROUTER otherwise */
    uint8_t routers;  /**< routers to query, set when discover ends */
    uint8_t queried;  /**< routers finished */
    int64_t end_ms;
    uint32_t gen;     /**< bumped by every scan start: cap and watchdog events carry it */
    uint16_t children_capacity[MESH_SCAN_MAX_ROUTERS];
    // Child address step
    ask_slot_t slots[MESH_SCAN_CHILD_QUERIES];
    uint8_t ask_router;        /**< cursor: the next child to ask is at or after (ask_router, ask_index) */
    uint16_t ask_index;
    uint16_t children_total;   /**< children picked for the step */
    uint16_t children_queried; /**< of those, answered or given up */
} s;

// Result memory

static void block_put(block_t *b)
{
    if (!b || --b->refs > 0) {
        return;
    }
    for (int id = 0; id < MESH_SCAN_MAX_ROUTERS; id++) {
        if (b->result.routers[id].children) {
            s.ops.release(b->result.routers[id].children);
        }
    }
    s.ops.release(b);
}

static void drop_result(void)
{
    block_put(s.block);
    s.block = NULL;
}

/** Make room for one more child of `r`; false when the cap is reached or memory is short. */
static bool reserve_child(mesh_scan_router_t *r)
{
    uint16_t *capacity = &s.children_capacity[r->router_id];

    if (r->n_children < *capacity) {
        return true;
    }
    if (*capacity >= MESH_SCAN_MAX_CHILDREN) {
        return false;
    }
    uint16_t grown = *capacity ? (uint16_t)(*capacity * 2) : FIRST_CHILDREN_CAPACITY;
    if (grown > MESH_SCAN_MAX_CHILDREN) {
        grown = MESH_SCAN_MAX_CHILDREN;
    }
    mesh_scan_child_t *children = s.ops.alloc(grown * sizeof(*children));
    if (!children) {
        return false;
    }
    if (r->n_children) {
        memcpy(children, r->children, r->n_children * sizeof(*children));
    }
    if (r->children) {
        s.ops.release(r->children);
    }
    r->children = children;
    *capacity = grown;
    return true;
}

// State changes

static void fail(mesh_scan_error_t error)
{
    s.ops.disarm_cap();
    drop_result();
    s.state = MESH_SCAN_FAILED;
    s.error = error;
    s.current = NO_ROUTER;
    s.end_ms = s.ops.now_ms();
    ESP_LOGW(TAG, "scan failed (error %d)", (int)error);
}

static void finish(bool cap_hit)
{
    mesh_scan_result_t *r = &s.block->result;
    bool partial = cap_hit;

    s.ops.disarm_cap();
    s.ops.snapshot(r);
    for (int id = 0; id < MESH_SCAN_MAX_ROUTERS; id++) {
        const mesh_scan_router_t *router = &r->routers[id];
        if (router->present && (!router->responded || !router->children_detail)) {
            partial = true;
        }
    }
    r->partial = partial;
    s.state = MESH_SCAN_DONE;
    s.current = NO_ROUTER;
    s.end_ms = s.ops.now_ms();
    ESP_LOGI(TAG, "scan done (%u routers queried%s)", (unsigned)s.queried, partial ? ", partial" : "");
}

/** Cap hit in the child address step: every child still owed an answer is not reported. */
static void mark_unanswered(void)
{
    mesh_scan_result_t *r = &s.block->result;

    for (int id = 0; id < MESH_SCAN_MAX_ROUTERS; id++) {
        for (uint16_t i = 0; i < r->routers[id].n_children; i++) {
            mesh_scan_child_t *c = &r->routers[id].children[i];
            if (c->ask_pending) {
                c->ask_pending = false;
                c->addresses_reported = false;
            }
        }
    }
    memset(s.slots, 0, sizeof(s.slots));
}

/**
 * Cap hit: the router being queried and every responding router not reached yet have no children detail; in the
 * child address step, the children still owed an answer are not reported.
 */
static void mark_unfinished(void)
{
    if (s.phase == PHASE_CHILD_DIAG) {
        mark_unanswered();
        return;
    }
    mesh_scan_result_t *r = &s.block->result;
    const int from = s.phase == PHASE_DISCOVER ? 0 : s.current;

    for (int id = from; id < MESH_SCAN_MAX_ROUTERS; id++) {
        if (r->routers[id].present && r->routers[id].responded) {
            r->routers[id].children_detail = false;
        }
    }
}

/** Close the router being queried without its children detail. */
static void skip_current(void)
{
    s.block->result.routers[s.current].children_detail = false;
    s.queried++;
}

/** Answer of a query start: true when it runs; otherwise the router is skipped or the scan failed. */
static bool query_started(int err)
{
    if (err == MESH_SCAN_OT_ERROR_NONE) {
        return true;
    }
    if (err == MESH_SCAN_OT_ERROR_INVALID_STATE) {
        fail(MESH_SCAN_ERR_STOPPED);
    } else {
        skip_current();
    }
    return false;
}

// Child address step (spec 3.4, ruling R14): rx-on children without addresses are asked directly, at most
// MESH_SCAN_CHILD_QUERIES at a time in router ID and child order, each with a deadline of MESH_SCAN_CHILD_DEADLINE_MS.

/** Close the child's query; `reported` false: no answer. */
static void close_child(mesh_scan_child_t *c, bool reported)
{
    c->ask_pending = false;
    c->addresses_reported = reported;
    s.children_queried++;
}

static void close_slot(ask_slot_t *slot, bool reported)
{
    close_child(slot->child, reported);
    slot->child = NULL;
}

/** The next child picked for the step and not asked yet, advancing the cursor past it; NULL when none is left. */
static mesh_scan_child_t *next_unasked(void)
{
    mesh_scan_result_t *r = &s.block->result;

    for (; s.ask_router < MESH_SCAN_MAX_ROUTERS; s.ask_router++, s.ask_index = 0) {
        mesh_scan_router_t *router = &r->routers[s.ask_router];
        while (s.ask_index < router->n_children) {
            mesh_scan_child_t *c = &router->children[s.ask_index++];
            if (c->ask_pending) {
                return c;
            }
        }
    }
    return NULL;
}

/** Arm the child deadline for the earliest query in flight. */
static void arm_deadline(void)
{
    int64_t due = INT64_MAX;

    for (int i = 0; i < MESH_SCAN_CHILD_QUERIES; i++) {
        if (s.slots[i].child && s.slots[i].due_ms < due) {
            due = s.slots[i].due_ms;
        }
    }
    if (due == INT64_MAX) {
        return;
    }
    const int64_t wait = due - s.ops.now_ms();
    s.ops.arm_child_deadline(wait > 0 ? (uint32_t)wait : 0, s.gen);
}

/** Fill the free slots with the next children and arm the deadline; finish once nothing is in flight or left. */
static void fill_slots(void)
{
    bool in_flight = false;

    for (int i = 0; i < MESH_SCAN_CHILD_QUERIES; i++) {
        ask_slot_t *slot = &s.slots[i];
        mesh_scan_child_t *c;

        while (!slot->child && (c = next_unasked()) != NULL) {
            int err = s.ops.query_child_diag(c->rloc16, s.gen);
            if (err == MESH_SCAN_OT_ERROR_NONE) {
                slot->child = c;
                slot->due_ms = s.ops.now_ms() + MESH_SCAN_CHILD_DEADLINE_MS;
            } else if (err == MESH_SCAN_OT_ERROR_INVALID_STATE) {
                fail(MESH_SCAN_ERR_STOPPED);
                return;
            } else {
                close_child(c, false); // NO_BUFS, BUSY: move on
            }
        }
        in_flight |= slot->child != NULL;
    }
    if (in_flight) {
        arm_deadline();
    } else {
        finish(false);
    }
}

/** After the last router: pick the rx-on children without addresses and start asking them. */
static void start_child_step(void)
{
    mesh_scan_result_t *r = &s.block->result;

    s.phase = PHASE_CHILD_DIAG;
    s.current = NO_ROUTER;
    s.ask_router = 0;
    s.ask_index = 0;
    memset(s.slots, 0, sizeof(s.slots));
    for (int id = 0; id < MESH_SCAN_MAX_ROUTERS; id++) {
        for (uint16_t i = 0; i < r->routers[id].n_children; i++) {
            mesh_scan_child_t *c = &r->routers[id].children[i];
            c->ask_pending = c->rx_on_when_idle && c->n_addrs == 0;
            if (c->ask_pending) {
                s.children_total++;
            }
        }
    }
    fill_slots();
}

/** Query the lowest responding router with an ID of `from` or above; the child address step after the last. */
static void next_router(int from)
{
    const mesh_scan_result_t *r = &s.block->result;

    for (int id = from; id < MESH_SCAN_MAX_ROUTERS; id++) {
        if (!r->routers[id].present || !r->routers[id].responded) {
            continue;
        }
        s.current = (uint8_t)id;
        s.phase = PHASE_CHILD_TABLE;
        if (query_started(s.ops.query_child_table(r->routers[id].rloc16))) {
            return;
        }
        if (s.state != MESH_SCAN_RUNNING) {
            return;
        }
    }
    start_child_step();
}

/** Every callback starts here: false when the callback must be ignored (not running, wrong phase, mode left). */
static bool callback_enter(phase_t phase)
{
    if (s.state != MESH_SCAN_RUNNING || s.phase != phase) {
        return false;
    }
    if (!s.ops.br_running()) {
        s.ops.cancel();
        fail(MESH_SCAN_ERR_STOPPED);
        return false;
    }
    return true;
}

static void lock(void)
{
    xSemaphoreTake(s.lock, portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGive(s.lock);
}

// Public entry points

void mesh_scan_set_ops(const mesh_scan_ops_t *ops)
{
    if (!s.lock) {
        s.lock = xSemaphoreCreateMutexStatic(&s.lock_storage);
    }
    lock();
    if (s.block) {
        drop_result(); // through the ops that allocated it
    }
    s.ops = *ops;
    s.state = MESH_SCAN_IDLE;
    s.error = MESH_SCAN_ERR_NONE;
    s.current = NO_ROUTER;
    s.routers = 0;
    s.queried = 0;
    unlock();
}

esp_err_t mesh_scan_core_start(void)
{
    if (!s.lock) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t ret = ESP_OK;

    lock();
    if (!s.ops.br_running() || s.state == MESH_SCAN_RUNNING) {
        ret = ESP_ERR_INVALID_STATE;
        goto out;
    }
    drop_result();
    memset(s.children_capacity, 0, sizeof(s.children_capacity));
    s.routers = 0;
    s.queried = 0;
    s.children_total = 0;
    s.children_queried = 0;
    s.current = NO_ROUTER;
    s.block = s.ops.alloc(sizeof(*s.block));
    if (!s.block) {
        s.state = MESH_SCAN_FAILED;
        s.error = MESH_SCAN_ERR_NO_MEMORY;
        s.end_ms = s.ops.now_ms();
        ESP_LOGW(TAG, "no memory for the result (%u bytes)", (unsigned)sizeof(*s.block));
        ret = ESP_ERR_NO_MEM;
        goto out;
    }
    s.block->refs = 1;
    s.state = MESH_SCAN_RUNNING;
    s.error = MESH_SCAN_ERR_NONE;
    s.phase = PHASE_DISCOVER;
    s.gen++;
    s.ops.arm_cap(MESH_SCAN_CAP_MS, s.gen);

    int err = s.ops.discover();
    if (err == MESH_SCAN_OT_ERROR_BUSY) {
        fail(MESH_SCAN_ERR_BUSY);
        ret = ESP_ERR_NOT_FINISHED;
    } else if (err != MESH_SCAN_OT_ERROR_NONE) {
        fail(MESH_SCAN_ERR_STOPPED);
        ret = ESP_FAIL;
    }
out:
    unlock();
    return ret;
}

void mesh_scan_core_cancel(void)
{
    if (!s.lock) {
        return;
    }
    lock();
    if (s.state == MESH_SCAN_RUNNING) {
        s.ops.cancel();
        fail(MESH_SCAN_ERR_STOPPED);
    }
    unlock();
}

/** The view; the caller holds the mutex. */
static void view_locked(mesh_scan_view_t *out)
{
    memset(out, 0, sizeof(*out));
    out->state = s.state;
    switch (s.state) {
    case MESH_SCAN_RUNNING:
        out->routers = s.routers;
        out->queried = s.queried;
        out->children_total = s.children_total;
        out->children_queried = s.children_queried;
        break;
    case MESH_SCAN_DONE:
    case MESH_SCAN_FAILED: {
        int64_t age_ms = s.ops.now_ms() - s.end_ms;
        out->age_s = age_ms > 0 ? (uint32_t)(age_ms / 1000) : 0;
        if (s.state == MESH_SCAN_DONE) {
            out->result = &s.block->result;
        } else {
            out->error = s.error;
        }
        break;
    }
    case MESH_SCAN_IDLE:
    default:
        break;
    }
}

void mesh_scan_view(mesh_scan_view_t *out)
{
    if (!s.lock) {
        *out = (mesh_scan_view_t) {
            .state = MESH_SCAN_IDLE
        };
        return;
    }
    lock();
    view_locked(out);
    unlock();
}

esp_err_t mesh_scan_render(mesh_scan_sink_t sink, void *ctx)
{
    mesh_scan_view_t view = { .state = MESH_SCAN_IDLE };
    block_t *held = NULL;

    if (s.lock) {
        lock();
        view_locked(&view);
        if (view.state == MESH_SCAN_DONE) {
            held = s.block;
            held->refs++;
        }
        unlock();
    }

    // Without the mutex: a slow client must not block the OpenThread task. The reference keeps the result alive.
    esp_err_t ret = mesh_scan_render_view(&view, sink, ctx);

    if (held) {
        lock();
        block_put(held);
        unlock();
    }
    return ret;
}

// Callbacks (OpenThread task)

void mesh_scan_on_router(const mesh_scan_router_t *in)
{
    lock();
    if (!callback_enter(PHASE_DISCOVER) || in->router_id >= MESH_SCAN_MAX_ROUTERS) {
        goto out;
    }
    mesh_scan_router_t *r = &s.block->result.routers[in->router_id];

    if (r->children) {
        s.ops.release(r->children); // a repeated report replaces the first
    }
    memset(r, 0, sizeof(*r));
    s.children_capacity[in->router_id] = 0;
    r->present = true;
    r->responded = true;
    r->children_detail = true;
    r->rloc16 = in->rloc16;
    r->router_id = in->router_id;
    memcpy(r->ext, in->ext, sizeof(r->ext));
    r->has_version = in->has_version;
    r->version = in->version;
    r->leader = in->leader;
    r->this_device = in->this_device;
    r->border_router = in->border_router;
    memcpy(r->lq_in, in->lq_in, sizeof(r->lq_in));
    r->n_addrs = in->n_addrs < MESH_SCAN_MAX_ROUTER_ADDRS ? in->n_addrs : MESH_SCAN_MAX_ROUTER_ADDRS;
    memcpy(r->addrs, in->addrs, r->n_addrs * sizeof(r->addrs[0]));

    uint16_t n = in->n_children;
    if (n > MESH_SCAN_MAX_CHILDREN) {
        n = MESH_SCAN_MAX_CHILDREN;
        r->children_detail = false;
    }
    if (n && in->children) {
        r->children = s.ops.alloc(n * sizeof(*r->children));
        if (!r->children) {
            r->children_detail = false;
            goto out;
        }
        s.children_capacity[in->router_id] = n;
        for (uint16_t i = 0; i < n; i++) {
            mesh_scan_child_t *c = &r->children[i];
            c->rloc16 = in->children[i].rloc16;
            c->rx_on_when_idle = in->children[i].rx_on_when_idle;
            c->full_thread_device = in->children[i].full_thread_device;
            c->full_network_data = in->children[i].full_network_data;
            c->lq_in = in->children[i].lq_in;
            c->addresses_reported = true;
        }
        r->n_children = n;
    }
out:
    unlock();
}

void mesh_scan_on_discover_done(int ot_error)
{
    uint16_t rloc16[MESH_SCAN_MAX_ROUTERS];
    uint8_t ids[MESH_SCAN_MAX_ROUTERS];

    lock();
    if (!callback_enter(PHASE_DISCOVER)) {
        goto out;
    }
    if (ot_error != MESH_SCAN_OT_ERROR_NONE) {
        ESP_LOGW(TAG, "discover ended with error %d; keeping what answered", ot_error);
    }
    mesh_scan_result_t *res = &s.block->result;

    s.routers = 0;
    for (int id = 0; id < MESH_SCAN_MAX_ROUTERS; id++) {
        if (res->routers[id].present) {
            s.routers++;
        }
    }
    uint8_t n = s.ops.router_table(rloc16, ids);
    for (uint8_t i = 0; i < n && i < MESH_SCAN_MAX_ROUTERS; i++) {
        if (ids[i] >= MESH_SCAN_MAX_ROUTERS || res->routers[ids[i]].present) {
            continue;
        }
        mesh_scan_router_t *stub = &res->routers[ids[i]];
        stub->present = true;
        stub->responded = false;
        stub->router_id = ids[i];
        stub->rloc16 = rloc16[i];
    }
    next_router(0);
out:
    unlock();
}

static mesh_scan_child_t *find_child(mesh_scan_router_t *r, uint16_t rloc16)
{
    for (uint16_t i = 0; i < r->n_children; i++) {
        if (r->children[i].rloc16 == rloc16) {
            return &r->children[i];
        }
    }
    return NULL;
}

void mesh_scan_on_child_entry(const mesh_scan_child_t *in)
{
    lock();
    if (!callback_enter(PHASE_CHILD_TABLE)) {
        goto out;
    }
    mesh_scan_router_t *r = &s.block->result.routers[s.current];
    mesh_scan_child_t *c = find_child(r, in->rloc16);

    if (!c) {
        if (!reserve_child(r)) {
            r->children_detail = false;
            goto out;
        }
        c = &r->children[r->n_children++];
        memset(c, 0, sizeof(*c));
        c->rloc16 = in->rloc16;
        c->rx_on_when_idle = in->rx_on_when_idle;
        c->full_thread_device = in->full_thread_device;
        c->full_network_data = in->full_network_data;
        c->lq_in = in->lq_in;
        c->addresses_reported = true;
    }
    memcpy(c->ext, in->ext, sizeof(c->ext));
    c->has_ext = true;
    c->timeout_s = in->timeout_s;
    c->age_s = in->age_s;
    c->connected_s = in->connected_s;
    c->rssi_avg = in->rssi_avg;
    c->rssi_last = in->rssi_last;
    c->link_margin_db = in->link_margin_db;
    c->has_err_rate = in->has_err_rate;
    c->frame_err = in->frame_err;
    c->msg_err = in->msg_err;
    c->version = in->version;
    c->csl = in->csl;
out:
    unlock();
}

void mesh_scan_on_child_table_done(int ot_error)
{
    lock();
    if (!callback_enter(PHASE_CHILD_TABLE)) {
        goto out;
    }
    if (ot_error != MESH_SCAN_OT_ERROR_NONE) {
        skip_current();
        next_router(s.current + 1);
        goto out;
    }
    s.phase = PHASE_CHILD_ADDRS;
    if (!query_started(s.ops.query_child_addrs(s.block->result.routers[s.current].rloc16))
            && s.state == MESH_SCAN_RUNNING) {
        next_router(s.current + 1);
    }
out:
    unlock();
}

void mesh_scan_on_child_addrs(uint16_t child_rloc16, const uint8_t (*addrs)[16], uint8_t n)
{
    lock();
    if (!callback_enter(PHASE_CHILD_ADDRS)) {
        goto out;
    }
    mesh_scan_child_t *c = find_child(&s.block->result.routers[s.current], child_rloc16);

    for (uint8_t i = 0; c && i < n && c->n_addrs < MESH_SCAN_MAX_CHILD_ADDRS; i++) {
        memcpy(c->addrs[c->n_addrs++], addrs[i], sizeof(c->addrs[0]));
    }
out:
    unlock();
}

void mesh_scan_on_child_addrs_done(int ot_error)
{
    lock();
    if (!callback_enter(PHASE_CHILD_ADDRS)) {
        goto out;
    }
    if (ot_error != MESH_SCAN_OT_ERROR_NONE) {
        s.block->result.routers[s.current].children_detail = false;
    }
    s.queried++;
    next_router(s.current + 1);
out:
    unlock();
}

void mesh_scan_on_child_diag(uint32_t gen, uint16_t peer_rloc16, const uint8_t (*addrs)[16], uint8_t n)
{
    lock();
    if (gen != s.gen || !callback_enter(PHASE_CHILD_DIAG)) {
        goto out; // a late answer of an older scan, or of this one after the step (otMeshDiagCancel can't stop it)
    }
    ask_slot_t *slot = NULL;
    for (int i = 0; i < MESH_SCAN_CHILD_QUERIES && !slot; i++) {
        if (s.slots[i].child && s.slots[i].child->rloc16 == peer_rloc16) {
            slot = &s.slots[i];
        }
    }
    if (!slot) {
        goto out; // no query to that peer in flight: given up already, never asked, or another sender's answer
    }
    mesh_scan_child_t *c = slot->child;
    for (uint8_t i = 0; i < n && c->n_addrs < MESH_SCAN_MAX_CHILD_ADDRS; i++) {
        memcpy(c->addrs[c->n_addrs++], addrs[i], sizeof(c->addrs[0]));
    }
    close_slot(slot, true);
    fill_slots();
out:
    unlock();
}

void mesh_scan_on_child_deadline(uint32_t gen)
{
    lock();
    if (gen != s.gen || !callback_enter(PHASE_CHILD_DIAG)) {
        goto out;
    }
    const int64_t now = s.ops.now_ms();
    bool closed = false;
    for (int i = 0; i < MESH_SCAN_CHILD_QUERIES; i++) {
        if (s.slots[i].child && s.slots[i].due_ms <= now) {
            close_slot(&s.slots[i], false);
            closed = true;
        }
    }
    if (closed) {
        fill_slots();
    } else {
        arm_deadline(); // early: armed for a query that has answered since
    }
out:
    unlock();
}

bool mesh_scan_rloc16_from_peer(const uint8_t peer[16], const uint8_t ml_prefix[8], uint16_t *rloc16)
{
    static const uint8_t RLOC_IID_HEAD[6] = { 0, 0, 0, 0xff, 0xfe, 0 };

    if (memcmp(peer, ml_prefix, 8) || memcmp(peer + 8, RLOC_IID_HEAD, sizeof(RLOC_IID_HEAD))) {
        return false;
    }
    *rloc16 = (uint16_t)(peer[14] << 8 | peer[15]);
    return true;
}

void mesh_scan_on_cap(void)
{
    lock();
    if (s.state != MESH_SCAN_RUNNING) {
        goto out;
    }
    s.ops.cancel();
    if (s.ops.br_running()) {
        ESP_LOGW(TAG, "scan cap of %d ms hit", MESH_SCAN_CAP_MS);
        mark_unfinished();
        finish(true);
    } else {
        fail(MESH_SCAN_ERR_STOPPED);
    }
out:
    unlock();
}

void mesh_scan_on_mode_lost(uint32_t gen)
{
    lock();
    if (s.state == MESH_SCAN_RUNNING && gen == s.gen) {
        ESP_LOGW(TAG, "mode left BR_RUNNING during the scan");
        fail(MESH_SCAN_ERR_STOPPED);
    }
    unlock();
}
