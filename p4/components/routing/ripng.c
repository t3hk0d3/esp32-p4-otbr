#include "ripng.h"

#include <string.h>

#include "announce.h"

#define CMD_REQUEST 1
#define CMD_RESPONSE 2
#define VERSION 1
#define PREFIX_LEN 64

/* RTE layout (RFC 2080 section 2.1): prefix 16, route tag 2, prefix length 1, metric 1. */
#define RTE_TAG_OFF 16
#define RTE_PLEN_OFF 18
#define RTE_METRIC_OFF 19

/* Wrap-safe: true once `now` has reached `at` (deadlines lie well within 2^31 ms of now). */
static bool due(uint32_t now, uint32_t at)
{
    return (int32_t)(now - at) >= 0;
}

/* now + delay, moved 1 ms later when it would equal UINT32_MAX, which next_deadline_ms reserves for "none". */
static uint32_t deadline(uint32_t now, uint32_t delay_ms)
{
    uint32_t at = now + delay_ms;

    return at == UINT32_MAX ? 0 : at;
}

static bool same(const routing_prefix64_t *a, const routing_prefix64_t *b)
{
    return memcmp(a->b, b->b, sizeof(a->b)) == 0;
}

static bool in_set(const routing_prefix_set_t *set, const routing_prefix64_t *p)
{
    for (size_t i = 0; i < set->count; i++) {
        if (same(&set->p[i], p)) {
            return true;
        }
    }
    return false;
}

/* --- codec --- */

size_t routing_ripng_encode_response(uint8_t *buf, size_t cap, const routing_ripng_rte_t *rtes, size_t n,
                                     size_t *consumed)
{
    *consumed = 0;
    if (cap < ROUTING_RIPNG_HDR_LEN + ROUTING_RIPNG_RTE_LEN) {
        return 0;
    }
    size_t fit = (cap - ROUTING_RIPNG_HDR_LEN) / ROUTING_RIPNG_RTE_LEN;
    size_t take = n < fit ? n : fit;

    if (take == 0) {
        return 0;
    }
    buf[0] = CMD_RESPONSE;
    buf[1] = VERSION;
    buf[2] = 0;
    buf[3] = 0;
    for (size_t i = 0; i < take; i++) {
        uint8_t *rte = &buf[ROUTING_RIPNG_HDR_LEN + i * ROUTING_RIPNG_RTE_LEN];

        memset(rte, 0, ROUTING_RIPNG_RTE_LEN);
        memcpy(rte, rtes[i].prefix.b, sizeof(rtes[i].prefix.b));
        rte[RTE_PLEN_OFF] = PREFIX_LEN;
        rte[RTE_METRIC_OFF] = rtes[i].metric;
    }
    *consumed = take;
    return ROUTING_RIPNG_HDR_LEN + take * ROUTING_RIPNG_RTE_LEN;
}

/* True for the whole-table request RTE: prefix ::, prefix length 0, metric 16 (RFC 2080 section 2.4.1). */
static bool is_whole_table_rte(const uint8_t *rte)
{
    for (size_t i = 0; i < RTE_TAG_OFF; i++) {
        if (rte[i] != 0) {
            return false;
        }
    }
    return rte[RTE_PLEN_OFF] == 0 && rte[RTE_METRIC_OFF] == ROUTING_RIPNG_INFINITY;
}

routing_ripng_kind_t routing_ripng_classify(const uint8_t *buf, size_t len)
{
    if (len < ROUTING_RIPNG_HDR_LEN || buf[1] != VERSION ||
            (len - ROUTING_RIPNG_HDR_LEN) % ROUTING_RIPNG_RTE_LEN != 0) {
        return ROUTING_RIPNG_INVALID;
    }
    switch (buf[0]) {
    case CMD_RESPONSE:
        return ROUTING_RIPNG_RESPONSE;
    case CMD_REQUEST:
        if (len == ROUTING_RIPNG_HDR_LEN + ROUTING_RIPNG_RTE_LEN && is_whole_table_rte(&buf[ROUTING_RIPNG_HDR_LEN])) {
            return ROUTING_RIPNG_REQUEST_ALL;
        }
        return ROUTING_RIPNG_REQUEST_SOME;
    default:
        return ROUTING_RIPNG_INVALID;
    }
}

size_t routing_ripng_answer_request(const uint8_t *req, size_t len, const routing_prefix_set_t *announced,
                                    routing_ripng_rte_t *out, size_t out_cap)
{
    size_t n = 0;

    switch (routing_ripng_classify(req, len)) {
    case ROUTING_RIPNG_REQUEST_ALL:
        for (size_t i = 0; i < announced->count && n < out_cap; i++) {
            out[n].prefix = announced->p[i];
            out[n++].metric = ROUTING_RIPNG_METRIC;
        }
        return n;
    case ROUTING_RIPNG_REQUEST_SOME:
        for (size_t off = ROUTING_RIPNG_HDR_LEN; off < len && n < out_cap; off += ROUTING_RIPNG_RTE_LEN) {
            const uint8_t *rte = &req[off];

            memcpy(out[n].prefix.b, rte, sizeof(out[n].prefix.b));
            out[n].metric = rte[RTE_PLEN_OFF] == PREFIX_LEN && in_set(announced, &out[n].prefix)
                            ? ROUTING_RIPNG_METRIC : ROUTING_RIPNG_INFINITY;
            n++;
        }
        return n;
    default:
        return 0;
    }
}

/* --- scheduler --- */

static void emit(routing_ripng_rte_t *out, size_t out_cap, size_t *n, const routing_prefix64_t *p, uint8_t metric)
{
    if (*n < out_cap) {
        out[*n].prefix = *p;
        out[*n].metric = metric;
        (*n)++;
    }
}

static void withdrawn_remove(routing_ripng_sched_t *s, size_t i)
{
    s->withdrawn[i] = s->withdrawn[--s->withdrawn_count];
}

static int withdrawn_find(const routing_ripng_sched_t *s, const routing_prefix64_t *p)
{
    for (size_t i = 0; i < s->withdrawn_count; i++) {
        if (same(&s->withdrawn[i].prefix, p)) {
            return (int)i;
        }
    }
    return -1;
}

/* Adds `p` with a fresh count; when the list is full, the entry with the fewest updates left gives way. */
static void withdrawn_add(routing_ripng_sched_t *s, const routing_prefix64_t *p)
{
    size_t slot = s->withdrawn_count;

    if (slot == ROUTING_RIPNG_MAX_WITHDRAWN) {
        slot = 0;
        for (size_t i = 1; i < s->withdrawn_count; i++) {
            if (s->withdrawn[i].left < s->withdrawn[slot].left) {
                slot = i;
            }
        }
    } else {
        s->withdrawn_count++;
    }
    s->withdrawn[slot] = (routing_ripng_withdrawn_t) {
        .prefix = *p, .left = ROUTING_RIPNG_WITHDRAW_UPDATES, .changed = true
    };
}

void routing_ripng_sched_start(routing_ripng_sched_t *s, const routing_prefix_set_t *set, uint32_t now_ms,
                               uint32_t (*rand32)(void))
{
    memset(s, 0, sizeof(*s));
    s->running = true;
    s->rand32 = rand32;
    s->announced = *set;
    if (s->announced.count > ROUTING_MAX_ANNOUNCED) {
        s->announced.count = ROUTING_MAX_ANNOUNCED;
    }
    s->regular_at = deadline(now_ms, 0);
}

void routing_ripng_sched_set_changed(routing_ripng_sched_t *s, const routing_prefix_set_t *set, uint32_t now_ms)
{
    routing_prefix_set_t added;
    routing_prefix_set_t removed;
    routing_prefix_set_t next = *set;
    bool next_changed[ROUTING_MAX_ANNOUNCED];

    if (!s->running) {
        return;
    }
    if (next.count > ROUTING_MAX_ANNOUNCED) {
        next.count = ROUTING_MAX_ANNOUNCED;
    }
    routing_announce_diff(&s->announced, &next, &added, &removed);
    if (added.count == 0 && removed.count == 0) {
        return;
    }
    for (size_t i = 0; i < removed.count; i++) {
        withdrawn_add(s, &removed.p[i]);
    }
    for (size_t i = 0; i < next.count; i++) {
        int w = withdrawn_find(s, &next.p[i]);

        if (w >= 0) {
            withdrawn_remove(s, (size_t)w); /* re-announced: metric 1 from now on */
        }
        next_changed[i] = true;
        for (size_t j = 0; j < s->announced.count; j++) {
            if (same(&s->announced.p[j], &next.p[i])) {
                next_changed[i] = s->announced_changed[j];
            }
        }
    }
    s->announced = next;
    memcpy(s->announced_changed, next_changed, sizeof(next_changed));

    if (!s->trigger_pending) {
        uint32_t earliest = s->last_trigger_ms + ROUTING_RIPNG_TRIGGER_GAP_MS;

        s->trigger_pending = true;
        s->trigger_at = s->triggered_once && !due(now_ms, earliest) ? deadline(earliest, 0) : deadline(now_ms, 0);
    }
}

/* A full update: every announced prefix at metric 1, every withdrawn one at 16. It also stands in for a pending
 * triggered update: entries not yet sent since their change count it as their triggered send. */
static size_t send_regular(routing_ripng_sched_t *s, uint32_t now_ms, routing_ripng_rte_t *out, size_t out_cap)
{
    size_t n = 0;

    for (size_t i = 0; i < s->announced.count; i++) {
        emit(out, out_cap, &n, &s->announced.p[i], ROUTING_RIPNG_METRIC);
        s->announced_changed[i] = false;
    }
    for (size_t i = 0; i < s->withdrawn_count;) {
        routing_ripng_withdrawn_t *w = &s->withdrawn[i];

        emit(out, out_cap, &n, &w->prefix, ROUTING_RIPNG_INFINITY);
        if (w->changed) {
            w->changed = false;
        } else if (--w->left == 0) {
            withdrawn_remove(s, i);
            continue;
        }
        i++;
    }
    s->trigger_pending = false;
    s->regular_at = deadline(now_ms, ROUTING_RIPNG_UPDATE_MS + s->rand32() % (ROUTING_RIPNG_UPDATE_JITTER_MS + 1));
    return n;
}

/* A triggered update: only the entries changed since they were last sent. */
static size_t send_triggered(routing_ripng_sched_t *s, uint32_t now_ms, routing_ripng_rte_t *out, size_t out_cap)
{
    size_t n = 0;

    for (size_t i = 0; i < s->announced.count; i++) {
        if (s->announced_changed[i]) {
            emit(out, out_cap, &n, &s->announced.p[i], ROUTING_RIPNG_METRIC);
            s->announced_changed[i] = false;
        }
    }
    for (size_t i = 0; i < s->withdrawn_count; i++) {
        if (s->withdrawn[i].changed) {
            emit(out, out_cap, &n, &s->withdrawn[i].prefix, ROUTING_RIPNG_INFINITY);
            s->withdrawn[i].changed = false;
        }
    }
    s->trigger_pending = false;
    s->triggered_once = true;
    s->last_trigger_ms = now_ms;
    return n;
}

size_t routing_ripng_sched_poll(routing_ripng_sched_t *s, uint32_t now_ms, routing_ripng_rte_t *out, size_t out_cap)
{
    if (!s->running) {
        return 0;
    }
    if (due(now_ms, s->regular_at)) {
        return send_regular(s, now_ms, out, out_cap);
    }
    if (s->trigger_pending && due(now_ms, s->trigger_at)) {
        return send_triggered(s, now_ms, out, out_cap);
    }
    return 0;
}

size_t routing_ripng_sched_stop(routing_ripng_sched_t *s, routing_ripng_rte_t *out, size_t out_cap)
{
    size_t n = 0;

    if (!s->running) {
        return 0;
    }
    for (size_t i = 0; i < s->announced.count; i++) {
        emit(out, out_cap, &n, &s->announced.p[i], ROUTING_RIPNG_INFINITY);
    }
    for (size_t i = 0; i < s->withdrawn_count; i++) {
        emit(out, out_cap, &n, &s->withdrawn[i].prefix, ROUTING_RIPNG_INFINITY);
    }
    memset(s, 0, sizeof(*s));
    return n;
}

uint32_t routing_ripng_sched_next_deadline_ms(const routing_ripng_sched_t *s)
{
    if (!s->running) {
        return UINT32_MAX;
    }
    if (s->trigger_pending && (int32_t)(s->trigger_at - s->regular_at) < 0) {
        return s->trigger_at;
    }
    return s->regular_at;
}
