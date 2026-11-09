// Decisions of the RCP proxy: the .local host test, the connect back-off, the target lookup's answers, the UDP peer lock, the accept rule, the
// first-bytes check of a TCP client, the raw write failure rule, the byte ring (moved from usb_serial) and the HDLC
// cutter of UDP framing. Pure: no ESP-IDF calls.
#include "rcp_proxy_core.h"

#include <string.h>

#include "radio_mux_hdlc.h"

#define MDNS_SUFFIX ".local"

/* ---- settings ---- */

static char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

bool rcp_proxy_host_mdns(const char *host, char *out, size_t cap)
{
    const size_t n = strlen(host);
    const size_t suffix = sizeof(MDNS_SUFFIX) - 1;

    if (n <= suffix || n - suffix >= cap) {
        return false;
    }
    for (size_t i = 0; i < suffix; i++) {
        if (lower(host[n - suffix + i]) != MDNS_SUFFIX[i]) {
            return false;
        }
    }
    memcpy(out, host, n - suffix);
    out[n - suffix] = '\0';
    return true;
}

/* ---- connect back-off ---- */

uint32_t rcp_proxy_backoff_next(uint32_t prev_s, int64_t connected_for_ms)
{
    if (prev_s == 0 || connected_for_ms >= RCP_PROXY_BACKOFF_RESET_MS) {
        return 1;
    }
    return prev_s >= RCP_PROXY_BACKOFF_MAX_S / 2 ? RCP_PROXY_BACKOFF_MAX_S : prev_s * 2;
}

/* ---- UDP peer lock ---- */

bool rcp_proxy_addr_eq(const rcp_proxy_addr_t *a, const rcp_proxy_addr_t *b)
{
    return a->port == b->port && a->scope == b->scope && memcmp(a->ip, b->ip, sizeof(a->ip)) == 0;
}

void rcp_proxy_refusal_init(rcp_proxy_refusal_t *r)
{
    memset(r, 0, sizeof(*r));
}

bool rcp_proxy_refusal_cached(const rcp_proxy_refusal_t *r, const rcp_proxy_addr_t *src, int64_t now_ms)
{
    return r->cached && now_ms >= r->at_ms && now_ms - r->at_ms < RCP_PROXY_REFUSAL_CACHE_MS &&
           rcp_proxy_addr_eq(&r->addr, src);
}

void rcp_proxy_refusal_note(rcp_proxy_refusal_t *r, const rcp_proxy_addr_t *src, const char *why, int64_t now_ms)
{
    r->cached = true;
    r->addr = *src;
    r->why = why;
    r->at_ms = now_ms;
}

uint32_t rcp_proxy_refusal_log(rcp_proxy_refusal_t *r, int64_t now_ms)
{
    r->count++;
    if (r->logged && now_ms >= r->log_ms && now_ms - r->log_ms < RCP_PROXY_REFUSAL_LOG_MS) {
        return 0;
    }
    const uint32_t n = r->count;

    r->logged = true;
    r->log_ms = now_ms;
    r->count = 0;
    return n;
}

void rcp_proxy_udp_lock_init(rcp_proxy_udp_lock_t *lock)
{
    memset(lock, 0, sizeof(*lock));
}

bool rcp_proxy_udp_lock_expire(rcp_proxy_udp_lock_t *lock, int64_t now_ms)
{
    if (!lock->locked || now_ms - lock->last_ms < RCP_PROXY_UDP_SILENCE_MS) {
        return false;
    }
    lock->locked = false;
    return true;
}

bool rcp_proxy_udp_lock_offer(rcp_proxy_udp_lock_t *lock, const rcp_proxy_addr_t *src, int64_t now_ms)
{
    rcp_proxy_udp_lock_expire(lock, now_ms);
    if (lock->barred && (now_ms < lock->bar_ms || now_ms - lock->bar_ms >= RCP_PROXY_UDP_BAR_MS)) {
        lock->barred = false;
    }
    if ((lock->locked && !rcp_proxy_addr_eq(&lock->peer, src)) ||
            (lock->barred && rcp_proxy_addr_eq(&lock->bar, src))) {
        lock->refused++;
        return false;
    }
    lock->locked = true;
    lock->peer = *src;
    lock->last_ms = now_ms;
    return true;
}

void rcp_proxy_udp_lock_kick(rcp_proxy_udp_lock_t *lock)
{
    lock->locked = false;
}

void rcp_proxy_udp_lock_bar(rcp_proxy_udp_lock_t *lock, int64_t now_ms)
{
    if (!lock->locked) {
        return;
    }
    lock->locked = false;
    lock->barred = true;
    lock->bar = lock->peer;
    lock->bar_ms = now_ms;
}

int64_t rcp_proxy_udp_lock_deadline(const rcp_proxy_udp_lock_t *lock)
{
    return lock->locked ? lock->last_ms + RCP_PROXY_UDP_SILENCE_MS : -1;
}

/* ---- new peer session (reset-handshake replay) ---- */

void rcp_proxy_peer_memo_init(rcp_proxy_peer_memo_t *m)
{
    memset(m, 0, sizeof(*m));
}

bool rcp_proxy_peer_new(rcp_proxy_peer_memo_t *m, const rcp_proxy_addr_t *peer)
{
    const bool fresh = !m->had || !rcp_proxy_addr_eq(&m->last, peer);

    m->had = true;
    m->last = *peer;
    return fresh;
}

void rcp_proxy_replay_init(rcp_proxy_replay_t *r)
{
    r->active = false;
    r->start_ms = 0;
}

void rcp_proxy_replay_start(rcp_proxy_replay_t *r, int64_t now_ms)
{
    r->active = true;
    r->start_ms = now_ms;
}

void rcp_proxy_replay_done(rcp_proxy_replay_t *r)
{
    r->active = false;
}

bool rcp_proxy_replay_blocks(rcp_proxy_replay_t *r, int64_t now_ms)
{
    if (r->active && (now_ms < r->start_ms || now_ms - r->start_ms >= RCP_PROXY_REPLAY_MAX_MS)) {
        r->active = false;
    }
    return r->active;
}

/* ---- connect target lookup ---- */

uint32_t rcp_proxy_lookup_ask(rcp_proxy_lookup_t *l, uint32_t *seq)
{
    if (++*seq == 0) {
        ++*seq; // 0 means none
    }
    l->pending = *seq;
    return l->pending;
}

void rcp_proxy_lookup_cancel(rcp_proxy_lookup_t *l)
{
    l->pending = 0;
}

bool rcp_proxy_lookup_waiting(const rcp_proxy_lookup_t *l)
{
    return l->pending != 0;
}

bool rcp_proxy_lookup_answer(rcp_proxy_lookup_t *l, uint32_t seq)
{
    if (seq == 0 || seq != l->pending) {
        return false;
    }
    l->pending = 0;
    return true;
}

/* ---- TCP accept rule ---- */

rcp_proxy_accept_t rcp_proxy_accept(bool slot_taken, bool held_silent, bool local_allowed, bool peer_allowed)
{
    if (!local_allowed || !peer_allowed || (slot_taken && !held_silent)) {
        return RCP_PROXY_REFUSE;
    }
    return slot_taken ? RCP_PROXY_REPLACE : RCP_PROXY_TAKE;
}

/* ---- first bytes of a TCP client ---- */

// Each with its space: the longest is RCP_PROXY_SNIFF_LEN.
static const char *const HTTP_METHODS[] = {
    "GET ", "POST ", "PUT ", "DELETE ", "HEAD ", "OPTIONS ", "PATCH ", "CONNECT ", "TRACE ",
};

rcp_proxy_sniff_t rcp_proxy_sniff(const uint8_t *d, size_t n, bool timed_out)
{
    bool prefix = false; // the bytes are still the start of some method

    if (n == 0) {
        return RCP_PROXY_SNIFF_WAIT; // judged by the first bytes the client sends, whenever they come
    }
    for (size_t i = 0; i < sizeof(HTTP_METHODS) / sizeof(HTTP_METHODS[0]); i++) {
        const size_t len = strlen(HTTP_METHODS[i]);
        const size_t cmp = n < len ? n : len;

        if (memcmp(d, HTTP_METHODS[i], cmp) != 0) {
            continue;
        }
        if (n >= len) {
            return RCP_PROXY_SNIFF_REFUSE;
        }
        prefix = true;
    }
    if (prefix && !timed_out && n < RCP_PROXY_SNIFF_LEN) {
        return RCP_PROXY_SNIFF_WAIT;
    }
    return RCP_PROXY_SNIFF_PASS;
}

static rcp_proxy_hold_act_t hold_judge(rcp_proxy_hold_t *h, bool timed_out)
{
    switch (rcp_proxy_sniff(h->buf, h->len, timed_out)) {
    case RCP_PROXY_SNIFF_PASS:
        h->deadline_ms = -1;
        return RCP_PROXY_HOLD_PASS;
    case RCP_PROXY_SNIFF_REFUSE:
        h->deadline_ms = -1;
        return RCP_PROXY_HOLD_REFUSE;
    default:
        return RCP_PROXY_HOLD_WAIT;
    }
}

void rcp_proxy_hold_start(rcp_proxy_hold_t *h)
{
    h->len = 0;
    h->deadline_ms = -1; // no timer until the first byte: a silent client is never promoted
}

bool rcp_proxy_hold_silent(const rcp_proxy_hold_t *h)
{
    return h->len == 0;
}

size_t rcp_proxy_hold_room(const rcp_proxy_hold_t *h)
{
    return RCP_PROXY_SNIFF_LEN - h->len;
}

rcp_proxy_hold_act_t rcp_proxy_hold_add(rcp_proxy_hold_t *h, const uint8_t *d, size_t n, int64_t now_ms)
{
    const size_t room = rcp_proxy_hold_room(h);

    if (n > room) {
        n = room;
    }
    if (n == 0) {
        return RCP_PROXY_HOLD_WAIT;
    }
    if (h->len == 0) {
        h->deadline_ms = now_ms + RCP_PROXY_SNIFF_MS; // counted from the first byte, not from the accept
    }
    memcpy(h->buf + h->len, d, n);
    h->len += n;
    return hold_judge(h, false);
}

rcp_proxy_hold_act_t rcp_proxy_hold_tick(rcp_proxy_hold_t *h, int64_t now_ms)
{
    if (h->deadline_ms < 0 || now_ms < h->deadline_ms) {
        return RCP_PROXY_HOLD_WAIT;
    }
    return hold_judge(h, true);
}

/* ---- raw write failure ---- */

rcp_proxy_tx_fail_t rcp_proxy_raw_tx_failed(esp_err_t err)
{
    return err == ESP_ERR_INVALID_STATE ? RCP_PROXY_TX_DISCARD : RCP_PROXY_TX_END;
}

/* ---- byte ring ---- */

void rcp_proxy_ring_init(rcp_proxy_ring_t *r, uint8_t *storage, size_t cap)
{
    r->data = storage;
    r->cap = cap;
    r->head = 0;
    r->len = 0;
}

bool rcp_proxy_ring_push(rcp_proxy_ring_t *r, const uint8_t *src, size_t n)
{
    if (n > r->cap - r->len) {
        return false;
    }
    const size_t tail = (r->head + r->len) % r->cap;
    size_t first = r->cap - tail;

    if (first > n) {
        first = n;
    }
    memcpy(&r->data[tail], src, first);
    memcpy(r->data, src + first, n - first);
    r->len += n;
    return true;
}

size_t rcp_proxy_ring_take(rcp_proxy_ring_t *r, uint8_t *out, size_t cap)
{
    const size_t n = r->len < cap ? r->len : cap;
    size_t first = r->cap - r->head;

    if (first > n) {
        first = n;
    }
    memcpy(out, &r->data[r->head], first);
    memcpy(out + first, r->data, n - first);
    r->head = (r->head + n) % r->cap;
    r->len -= n;
    return n;
}

size_t rcp_proxy_ring_peek(const rcp_proxy_ring_t *r, const uint8_t **p)
{
    const size_t run = r->cap - r->head;

    *p = &r->data[r->head];
    return r->len < run ? r->len : run;
}

void rcp_proxy_ring_drop(rcp_proxy_ring_t *r, size_t n)
{
    if (n > r->len) {
        n = r->len;
    }
    r->head = (r->head + n) % r->cap;
    r->len -= n;
}

void rcp_proxy_ring_clear(rcp_proxy_ring_t *r)
{
    r->head = 0;
    r->len = 0;
}

/* ---- HDLC cutter (UDP framing hdlc on a byte channel) ---- */

void rcp_proxy_cut_init(rcp_proxy_cut_t *c, uint8_t *buf, size_t cap)
{
    c->buf = buf;
    c->cap = cap;
    c->len = 0;
    c->overflows = 0;
}

void rcp_proxy_cut_clear(rcp_proxy_cut_t *c)
{
    c->len = 0;
}

// Emits every complete frame in the buffer and moves what is left to the front.
static size_t cut_emit(rcp_proxy_cut_t *c, rcp_proxy_emit_fn emit, void *ctx)
{
    size_t frames = 0;
    size_t off = 0;
    size_t frame_len;
    size_t drop;

    while ((drop = radio_mux_hdlc_chunk(c->buf + off, c->len - off, &frame_len)) > 0) {
        emit(ctx, c->buf + off + drop - frame_len + 1, frame_len);
        frames++;
        off += drop;
    }
    if (off > 0) {
        memmove(c->buf, c->buf + off, c->len - off);
        c->len -= off;
    }
    return frames;
}

size_t rcp_proxy_cut_feed(rcp_proxy_cut_t *c, const uint8_t *in, size_t n, rcp_proxy_emit_fn emit, void *ctx)
{
    size_t frames = 0;

    while (n > 0) {
        size_t room = c->cap - c->len;

        if (room == 0) { // full without a complete frame
            const bool flag_last = c->buf[c->len - 1] == RADIO_MUX_HDLC_FLAG;

            c->buf[0] = RADIO_MUX_HDLC_FLAG;
            c->len = flag_last ? 1 : 0;
            c->overflows++;
            room = c->cap - c->len;
        }
        const size_t take = n < room ? n : room;

        memcpy(c->buf + c->len, in, take);
        c->len += take;
        in += take;
        n -= take;
        frames += cut_emit(c, emit, ctx);
    }
    return frames;
}

rcp_proxy_reinit_t rcp_proxy_reinit_verdict(bool peer_connected)
{
    return peer_connected ? RCP_PROXY_REINIT_REFUSE_PEER : RCP_PROXY_REINIT_ACCEPT;
}

bool rcp_proxy_input_current(uint32_t seen_seq, uint32_t input_seq)
{
    return (int32_t)(input_seq - seen_seq) > 0;
}

bool rcp_proxy_bind_after_reinit(bool accepted, uint32_t bind_seq, uint32_t bind_seen)
{
    return !accepted && rcp_proxy_input_current(bind_seen, bind_seq);
}
