#include "scan.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "render.h"
#include "srp.h"
#include "srp_step.h"

static const char *TAG = "mesh_scan";

#define NO_ROUTER 0xff
#define FIRST_CHILDREN_CAPACITY 8

typedef enum {
    PHASE_DISCOVER,
    PHASE_CHILD_TABLE,
    PHASE_CHILD_ADDRS,
    PHASE_CHILD_DIAG, /**< asking rx-on children without addresses directly (spec 3.4) */
    PHASE_SRP_WAIT,   /**< mesh phases over, waiting for the SRP step */
} phase_t;

/** A child address query in flight. */
typedef struct {
    mesh_scan_child_t *child; /**< NULL: the slot is free */
    int64_t due_ms;
} ask_slot_t;

/** Service types asked on the other SRP servers, in this order. */
static const char *const SRP_TYPES[] = {
    "_matter._tcp", "_matterc._udp", "_esphomelib._tcp", "_hap._udp", "_http._tcp", "_meshcop._udp", "_trel._udp",
};
#define N_SRP_TYPES (sizeof(SRP_TYPES) / sizeof(SRP_TYPES[0]))
#define MAX_SRP_JOBS (MESH_SCAN_MAX_SRP_SERVERS * N_SRP_TYPES + MESH_SCAN_SRP_RESOLVES)

typedef enum {
    JOB_QUEUED,
    JOB_IN_FLIGHT,
    JOB_OVER, /**< done, failed to start or dropped */
} job_state_t;

/** One DNS query of the SRP step; its index is the `job` the callbacks carry. */
typedef struct {
    bool resolve;     /**< false: browse */
    uint8_t server;   /**< index into the result's srp_servers */
    uint8_t type;     /**< index into SRP_TYPES */
    job_state_t state;
    char instance[MESH_SCAN_NAME_LEN]; /**< resolve only */
} srp_job_t;

/**
 * The SRP step of one scan: its queries and the staged records. Allocated at start when the network data lists a
 * remote server, released at finish (after the merge) or when the scan fails.
 */
typedef struct {
    mesh_scan_stage_t stage;
    srp_job_t jobs[MAX_SRP_JOBS]; /**< browses first, in round-robin over the servers; resolves appended */
    uint16_t n_jobs;
    uint16_t queued;              /**< jobs in state JOB_QUEUED */
    uint8_t in_flight;
    uint8_t resolves;             /**< resolves queued so far */
    uint8_t server_in_flight[MESH_SCAN_MAX_SRP_SERVERS];
    bool answered[MESH_SCAN_MAX_SRP_SERVERS]; /**< a query to that server got an answer */
    bool dead[MESH_SCAN_MAX_SRP_SERVERS];     /**< given up: timed out before any answer, its queued jobs dropped */
    bool cut[MESH_SCAN_MAX_SRP_SERVERS];      /**< that server still had queries when the step was cut */
} srp_step_t;

/**
 * One scan's result. The scan holds one reference while it is current; each render in flight holds another. The
 * block, its pool and its children arrays are released with the last reference.
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
    // SRP step
    srp_step_t *srp;  /**< NULL without a remote server, once released, or when it could not be allocated */
    bool srp_over;    /**< the SRP step has ended (or never had anything to do) */
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
    if (b->result.pool) {
        s.ops.release(b->result.pool);
    }
    s.ops.release(b);
}

static void drop_result(void)
{
    block_put(s.block);
    s.block = NULL;
}

static void drop_srp(void)
{
    if (s.srp) {
        s.ops.release(s.srp);
        s.srp = NULL;
    }
    s.srp_over = true;
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
    drop_srp(); // later DNS callbacks find no step
    memset(s.slots, 0, sizeof(s.slots)); // no pointer into the freed children arrays survives
    s.state = MESH_SCAN_FAILED;
    s.error = error;
    s.current = NO_ROUTER;
    s.end_ms = s.ops.now_ms();
    ESP_LOGW(TAG, "scan failed (error %d)", (int)error);
}

static void srp_cut(void);

static void finish(bool cap_hit)
{
    mesh_scan_result_t *r = &s.block->result;
    bool partial = cap_hit && s.phase != PHASE_SRP_WAIT; // the SRP step never makes a result partial

    if (!s.srp_over) {
        srp_cut(); // the cap
    }
    s.ops.disarm_cap();
    s.ops.snapshot(r);
    for (uint8_t i = 0; i < r->n_srp_servers; i++) {
        if (r->srp_servers[i].this_board) {
            r->srp_servers[i].hosts = r->n_srp; // before the merge
        }
    }
    if (s.srp) {
        mesh_scan_stage_merge(&s.srp->stage, r); // after this board's hosts, so their fields win
    }
    drop_srp();
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
    if (s.phase == PHASE_SRP_WAIT) {
        return;
    }
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

/** The last mesh phase ended: finish, or wait for the SRP step. */
static void mesh_phases_over(void)
{
    s.current = NO_ROUTER;
    if (s.srp_over) {
        finish(false);
    } else {
        s.phase = PHASE_SRP_WAIT;
    }
}

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
        mesh_phases_over();
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

// SRP step: from the start, in parallel with the mesh phases, browse SRP_TYPES on every remote SRP server, at most
// MESH_SCAN_SRP_QUERIES queries at a time (MESH_SCAN_SRP_SERVER_QUERIES per server while several are live), and resolve
// the instances a browse answer does not describe (at most MESH_SCAN_SRP_RESOLVES, MESH_SCAN_SRP_NAME_RESOLVES of them
// kept for the name-bearing types). A server whose query times out before it ever answered is given up: its queued
// jobs are dropped. Ends when every query is over or MESH_SCAN_SRP_BUDGET_MS after the start, whichever is first.

/**
 * Final server states: no_answer when nothing it was asked got an answer, or it was given up; cut_short when the cut
 * (or, after a late answer, giving it up) left it with jobs; answered otherwise.
 */
static void srp_close(void)
{
    mesh_scan_result_t *r = &s.block->result;

    for (uint8_t i = 0; i < r->n_srp_servers; i++) {
        mesh_scan_srp_server_t *srv = &r->srp_servers[i];
        if (srv->this_board) {
            srv->state = MESH_SCAN_SRP_ANSWERED; // read locally at finish
        } else if (!s.srp) {
            srv->state = MESH_SCAN_SRP_NO_ANSWER;
        } else if (s.srp->answered[i]) {
            srv->state = s.srp->cut[i] || s.srp->dead[i] ? MESH_SCAN_SRP_CUT_SHORT : MESH_SCAN_SRP_ANSWERED;
        } else {
            srv->state = s.srp->cut[i] && !s.srp->dead[i] ? MESH_SCAN_SRP_CUT_SHORT : MESH_SCAN_SRP_NO_ANSWER;
        }
    }
    s.srp_over = true;
}

/** End the step now (budget or cap): the queries in flight or queued are dropped, their servers marked cut. */
static void srp_cut(void)
{
    for (uint16_t i = 0; i < s.srp->n_jobs; i++) {
        srp_job_t *job = &s.srp->jobs[i];
        if (job->state != JOB_OVER) {
            s.srp->cut[job->server] = true;
            job->state = JOB_OVER;
        }
    }
    s.srp->queued = 0;
    s.srp->in_flight = 0;
    memset(s.srp->server_in_flight, 0, sizeof(s.srp->server_in_flight));
    srp_close();
}

/** The step has ended: finish if the mesh phases have too. */
static void srp_ended(void)
{
    if (s.phase == PHASE_SRP_WAIT) {
        finish(false);
    }
}

/** True for the types whose records carry a device name: ESPHome and HomeKit. */
static bool name_type(uint8_t type)
{
    return strcmp(SRP_TYPES[type], "_esphomelib._tcp") == 0 || strcmp(SRP_TYPES[type], "_hap._udp") == 0;
}

/** Issue order: browses, then resolves of the name-bearing types, then the rest. */
static uint8_t job_rank(const srp_job_t *job)
{
    if (!job->resolve) {
        return 0;
    }
    return name_type(job->type) ? 1 : 2;
}

/** Queries in flight per server at most: MESH_SCAN_SRP_SERVER_QUERIES while more than one remote server is live. */
static uint8_t srp_server_cap(const srp_step_t *st)
{
    const mesh_scan_result_t *r = &s.block->result;
    uint8_t live = 0;

    for (uint8_t i = 0; i < r->n_srp_servers; i++) {
        live += !r->srp_servers[i].this_board && !st->dead[i];
    }
    return live > 1 ? MESH_SCAN_SRP_SERVER_QUERIES : MESH_SCAN_SRP_QUERIES;
}

/** The queued job to issue next: lowest rank, then queue order, on a server below its cap. -1 when there is none. */
static int srp_next_job(const srp_step_t *st)
{
    const uint8_t cap = srp_server_cap(st);
    int best = -1;

    for (uint16_t i = 0; i < st->n_jobs; i++) {
        const srp_job_t *job = &st->jobs[i];
        if (job->state == JOB_QUEUED && st->server_in_flight[job->server] < cap &&
                (best < 0 || job_rank(job) < job_rank(&st->jobs[best]))) {
            best = i;
        }
    }
    return best;
}

/** Issue queued jobs while there is room; end the step once nothing is in flight or queued. Last thing a caller does. */
static void srp_pump(void)
{
    srp_step_t *st = s.srp;
    const mesh_scan_result_t *r = &s.block->result;
    int id;

    while (st->in_flight < MESH_SCAN_SRP_QUERIES && (id = srp_next_job(st)) >= 0) {
        srp_job_t *job = &st->jobs[id];
        const mesh_scan_srp_server_t *srv = &r->srp_servers[job->server];
        const char *type = SRP_TYPES[job->type];
        int err = job->resolve ? s.ops.dns_resolve(srv, job->instance, type, s.gen, (uint16_t)id)
                  : s.ops.dns_browse(srv, type, s.gen, (uint16_t)id);

        st->queued--;
        if (err == MESH_SCAN_OT_ERROR_NONE) {
            job->state = JOB_IN_FLIGHT;
            st->in_flight++;
            st->server_in_flight[job->server]++;
        } else {
            job->state = JOB_OVER; // NO_BUFS and the like: counts as done, without an answer
        }
    }
    if (st->in_flight == 0 && st->queued == 0) {
        srp_close();
        srp_ended();
    }
}

/** At scan start: list the servers, queue a browse of every type on every remote one and start the first queries. */
static void srp_start(void)
{
    mesh_scan_result_t *r = &s.block->result;
    uint8_t ext_pan_id[8] = { 0 };
    uint8_t own_ext[8] = { 0 };
    uint8_t remote = 0;

    uint8_t n = s.ops.srp_servers(r->srp_servers, &r->network, ext_pan_id, own_ext);
    r->n_srp_servers = n < MESH_SCAN_MAX_SRP_SERVERS ? n : MESH_SCAN_MAX_SRP_SERVERS;
    for (uint8_t i = 0; i < r->n_srp_servers; i++) {
        r->srp_servers[i].hosts = 0;
        remote += !r->srp_servers[i].this_board;
    }
    s.srp_over = false;
    if (remote == 0) {
        srp_close();
        return;
    }
    s.srp = s.ops.alloc(sizeof(*s.srp));
    if (!s.srp) {
        ESP_LOGW(TAG, "no memory for the SRP step (%u bytes); other servers not asked", (unsigned)sizeof(*s.srp));
        srp_close(); // every remote server: no_answer
        return;
    }
    memcpy(s.srp->stage.ext_pan_id, ext_pan_id, sizeof(ext_pan_id));
    memcpy(s.srp->stage.own_ext, own_ext, sizeof(own_ext));
    // Round-robin over the servers, and with several a per-server cap, so a silent one holds at most two slots.
    for (uint8_t t = 0; t < N_SRP_TYPES; t++) {
        for (uint8_t i = 0; i < r->n_srp_servers; i++) {
            if (!r->srp_servers[i].this_board) {
                s.srp->jobs[s.srp->n_jobs++] = (srp_job_t) {
                    .server = i, .type = t, .state = JOB_QUEUED
                };
                s.srp->queued++;
            }
        }
    }
    s.ops.arm_srp_deadline(MESH_SCAN_SRP_BUDGET_MS, s.gen);
    srp_pump();
}

/** Every SRP callback starts here: false when it must be ignored (not running, older scan, step over, mode left). */
static bool srp_enter(uint32_t gen)
{
    if (s.state != MESH_SCAN_RUNNING || gen != s.gen || s.srp_over || !s.srp) {
        return false;
    }
    if (!s.ops.br_running()) {
        fail(MESH_SCAN_ERR_STOPPED);
        return false;
    }
    return true;
}

/** The job `id` when it is in flight; NULL otherwise. */
static srp_job_t *srp_job_in_flight(uint16_t id)
{
    return id < s.srp->n_jobs && s.srp->jobs[id].state == JOB_IN_FLIGHT ? &s.srp->jobs[id] : NULL;
}

/** Every callback starts here: false when the callback must be ignored (not running, wrong phase, mode left). */
static bool callback_enter(phase_t phase)
{
    if (s.state != MESH_SCAN_RUNNING || s.phase != phase) {
        return false;
    }
    if (!s.ops.br_running()) {
        // No otMeshDiagCancel from inside a mesh-diag callback: OpenThread's single slot keeps a new discover out
        // (OT_ERROR_BUSY) until the abandoned one ends, as the watchdog path relies on.
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
    drop_srp();
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
    if (!s.ops.br_running()) {
        ret = ESP_ERR_NOT_ALLOWED;
        goto out;
    }
    if (s.state == MESH_SCAN_RUNNING) {
        ret = ESP_ERR_INVALID_STATE;
        goto out;
    }
    drop_result();
    drop_srp();
    memset(s.slots, 0, sizeof(s.slots));
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
    s.block->result.pool = s.ops.alloc(MESH_SCAN_POOL_SIZE);
    if (!s.block->result.pool) {
        drop_result();
        s.state = MESH_SCAN_FAILED;
        s.error = MESH_SCAN_ERR_NO_MEMORY;
        s.end_ms = s.ops.now_ms();
        ESP_LOGW(TAG, "no memory for the pool (%u bytes)", (unsigned)MESH_SCAN_POOL_SIZE);
        ret = ESP_ERR_NO_MEM;
        goto out;
    }
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
    } else {
        srp_start();
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

void mesh_scan_core_fail_stopped(void)
{
    if (!s.lock) {
        return;
    }
    lock();
    if (s.state == MESH_SCAN_RUNNING) {
        ESP_LOGW(TAG, "scan stopped without telling OpenThread");
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
    if (n == 0 || gen != s.gen || !callback_enter(PHASE_CHILD_DIAG)) {
        // No address list (e.g. an answer to the REST API's own query), or a late answer of an older scan, or of this
        // one after the step (otMeshDiagCancel can't stop it): not ours to close a child with.
        goto out;
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

/**
 * True when a resolve of `rec` cannot make it kept: it has addresses and none passes the address filter (a LAN host
 * from a discovery proxy). Border router types are judged by TXT, never by address.
 */
static bool lan_only(const char *type, const mesh_scan_srp_record_t *rec)
{
    const mesh_scan_network_t *net = &s.block->result.network;
    const uint8_t n = rec->n_addrs < MESH_SCAN_MAX_SRP_ADDRS ? rec->n_addrs : MESH_SCAN_MAX_SRP_ADDRS;

    if (strcmp(type, "_meshcop._udp") == 0 || strcmp(type, "_trel._udp") == 0 || n == 0) {
        return false;
    }
    return !mesh_scan_srp_addr_kept(rec->addrs, n, net->omr, net->n_omr, net->ml_prefix);
}

/** A server that never answered timed out: drop its queued jobs, so it holds no more slots. */
static void srp_give_up(uint8_t server)
{
    srp_step_t *st = s.srp;

    st->dead[server] = true;
    for (uint16_t i = 0; i < st->n_jobs; i++) {
        if (st->jobs[i].server == server && st->jobs[i].state == JOB_QUEUED) {
            st->jobs[i].state = JOB_OVER;
            st->queued--;
        }
    }
    ESP_LOGW(TAG, "SRP server 0x%04x did not answer; not asked again", s.block->result.srp_servers[server].rloc16);
}

void mesh_scan_on_srp_record(uint32_t gen, uint16_t id, const mesh_scan_srp_record_t *rec)
{
    lock();
    if (!srp_enter(gen)) {
        goto out;
    }
    srp_job_t *job = srp_job_in_flight(id);
    if (!job) {
        goto out; // given up at the deadline, already done, or never issued
    }
    srp_step_t *st = s.srp;
    // The name-bearing types may use every resolve; the others leave MESH_SCAN_SRP_NAME_RESOLVES for them, since the
    // count is spent at queue time and Matter browses answer first.
    const uint8_t room = name_type(job->type) ? MESH_SCAN_SRP_RESOLVES
                         : MESH_SCAN_SRP_RESOLVES - MESH_SCAN_SRP_NAME_RESOLVES;
    if (!job->resolve && !rec->described && rec->instance[0] && st->resolves < room && !st->dead[job->server] &&
            !lan_only(SRP_TYPES[job->type], rec)) {
        srp_job_t *next = &st->jobs[st->n_jobs++]; // room: MAX_SRP_JOBS counts every browse and resolve
        *next = (srp_job_t) {
            .resolve = true, .server = job->server, .type = job->type, .state = JOB_QUEUED
        };
        strncpy(next->instance, rec->instance, sizeof(next->instance) - 1);
        st->resolves++;
        st->queued++;
        goto out; // issued once a slot frees up
    }
    mesh_scan_stage_record(&st->stage, &s.block->result, job->server, SRP_TYPES[job->type], rec);
out:
    unlock();
}

void mesh_scan_on_srp_query_done(uint32_t gen, uint16_t id, int ot_error)
{
    lock();
    if (!srp_enter(gen)) {
        goto out;
    }
    srp_job_t *job = srp_job_in_flight(id);
    if (!job) {
        goto out;
    }
    job->state = JOB_OVER;
    s.srp->in_flight--;
    s.srp->server_in_flight[job->server]--;
    if (ot_error == MESH_SCAN_OT_ERROR_NONE || ot_error == MESH_SCAN_OT_ERROR_NOT_FOUND) {
        s.srp->answered[job->server] = true;
    } else if (ot_error == MESH_SCAN_OT_ERROR_RESPONSE_TIMEOUT && !s.srp->answered[job->server] &&
               !s.srp->dead[job->server]) {
        srp_give_up(job->server);
    }
    srp_pump();
out:
    unlock();
}

void mesh_scan_on_srp_deadline(uint32_t gen)
{
    lock();
    if (!srp_enter(gen)) {
        goto out;
    }
    ESP_LOGW(TAG, "SRP budget of %d ms used up; %u queries in flight", MESH_SCAN_SRP_BUDGET_MS,
             (unsigned)s.srp->in_flight);
    srp_cut();
    srp_ended();
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
