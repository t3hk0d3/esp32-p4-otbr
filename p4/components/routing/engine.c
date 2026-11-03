// Routing engine (engine.h): the routing task's decisions, pure and host-tested.
#include "engine.h"

#include <stdio.h>
#include <string.h>

#include "settings.h"

#define FEED_ROUNDS 4 /* an event plus the follow-ups its actions raise (connect failed, connection lost) */

/* Wrap-safe: true once `now` has reached `at`. */
static bool due(uint32_t now, uint32_t at)
{
    return (int32_t)(now - at) >= 0;
}

static bool prefix_equal(const routing_prefix64_t *a, const routing_prefix64_t *b)
{
    return memcmp(a->b, b->b, sizeof(a->b)) == 0;
}

/* --- connections --- */

static void conn_reset(routing_engine_conn_t *c)
{
    c->h = -1;
    c->connecting = false;
    c->len = 0;
}

static routing_engine_conn_t *cur(routing_engine_peer_t *p)
{
    return &p->conn[p->cur];
}

static routing_engine_conn_t *pend(routing_engine_peer_t *p)
{
    return &p->conn[p->cur ^ 1];
}

static void close_conn(routing_engine_t *e, routing_engine_conn_t *c)
{
    if (c->h >= 0) {
        e->io->tcp_close(e->io->ctx, c->h);
    }
    conn_reset(c);
}

static routing_engine_peer_t *peer_by_handle(routing_engine_t *e, int h)
{
    for (int i = 0; i < ROUTING_MAX_PEERS; i++) {
        routing_engine_peer_t *p = &e->peers[i];
        if (p->in_use && h >= 0 && (p->conn[0].h == h || p->conn[1].h == h)) {
            return p;
        }
    }
    return NULL;
}

static routing_engine_peer_t *peer_by_addr(routing_engine_t *e, const uint8_t addr[16])
{
    for (int i = 0; i < ROUTING_MAX_PEERS; i++) {
        if (e->peers[i].in_use && memcmp(e->peers[i].cfg.addr, addr, 16) == 0) {
            return &e->peers[i];
        }
    }
    return NULL;
}

static const routing_engine_peer_t *peer_by_addr_const(const routing_engine_t *e, const uint8_t addr[16])
{
    return peer_by_addr((routing_engine_t *)e, addr);
}

/* --- BGP actions --- */

typedef struct {
    bool connect_failed;
    const char *why;
    int lost_h; /**< a send on this current connection failed, -1 none */
} followup_t;

static void send_current(routing_engine_t *e, routing_engine_peer_t *p, size_t len, followup_t *f)
{
    routing_engine_conn_t *c = cur(p);

    if (len == 0 || c->h < 0 || c->connecting) {
        return;
    }
    if (e->io->tcp_send(e->io->ctx, c->h, e->tx, len) != 0) {
        f->lost_h = c->h;
    }
}

static void send_diff(routing_engine_t *e, routing_engine_peer_t *p, followup_t *f)
{
    routing_prefix_set_t added;
    routing_prefix_set_t removed;
    bool ibgp = p->cfg.asn == e->settings.local_asn;

    routing_announce_diff(&p->sent, &e->announced, &added, &removed);
    /* an empty withdrawal would be the End-of-RIB marker: only withdraw something */
    if (removed.count > 0) {
        send_current(e, p, routing_bgp_encode_withdraw(e->tx, sizeof(e->tx), &removed), f);
    }
    send_current(e, p, routing_bgp_encode_update(e->tx, sizeof(e->tx), ibgp, e->settings.local_asn,
                                                 &e->state.nexthop, &added), f);
    p->sent = e->announced;
    p->sent_nh = e->state.nexthop;
}

static void execute(routing_engine_t *e, routing_engine_peer_t *p, const routing_bgp_action_t *a, followup_t *f)
{
    bool ibgp = p->cfg.asn == e->settings.local_asn;
    routing_engine_conn_t *c;

    switch (a->type) {
    case ROUTING_BGP_ACT_CONNECT:
        c = cur(p);
        close_conn(e, c);
        c->h = e->io->tcp_connect(e->io->ctx, p->cfg.addr, &f->why);
        if (c->h < 0) {
            conn_reset(c);
            f->connect_failed = true;
        } else {
            c->connecting = true;
        }
        break;
    case ROUTING_BGP_ACT_SEND_OPEN:
        send_current(e, p, routing_bgp_encode_open(e->tx, sizeof(e->tx), e->settings.local_asn,
                                                   e->settings.hold_time, e->router_id), f);
        break;
    case ROUTING_BGP_ACT_SEND_KEEPALIVE:
        send_current(e, p, routing_bgp_encode_keepalive(e->tx, sizeof(e->tx)), f);
        break;
    case ROUTING_BGP_ACT_SEND_NOTIFICATION:
        send_current(e, p, routing_bgp_encode_notification(e->tx, sizeof(e->tx), a->notification.code,
                                                           a->notification.subcode), f);
        break;
    case ROUTING_BGP_ACT_SEND_FULL_SET:
        /* the codec returns 0 for an empty set: nothing to send, not an error */
        send_current(e, p, routing_bgp_encode_update(e->tx, sizeof(e->tx), ibgp, e->settings.local_asn,
                                                     &e->state.nexthop, &e->announced), f);
        p->sent = e->announced;
        p->sent_nh = e->state.nexthop;
        break;
    case ROUTING_BGP_ACT_SEND_DIFF:
        send_diff(e, p, f);
        break;
    case ROUTING_BGP_ACT_CLOSE:
        close_conn(e, cur(p));
        break;
    case ROUTING_BGP_ACT_ADOPT_INCOMING:
        close_conn(e, cur(p));
        p->cur ^= 1;
        break;
    case ROUTING_BGP_ACT_REJECT_INCOMING:
        c = pend(p);
        if (c->h >= 0 && a->notification.code != 0) {
            size_t len = routing_bgp_encode_notification(e->tx, sizeof(e->tx), a->notification.code,
                                                         a->notification.subcode);
            (void)e->io->tcp_send(e->io->ctx, c->h, e->tx, len); /* closed next: a failure changes nothing */
        }
        close_conn(e, c);
        break;
    }
}

/* Feeds `ev` to the peer's FSM and carries out its actions, then the follow-up events they raised. */
static void feed(routing_engine_t *e, routing_engine_peer_t *p, const routing_bgp_event_t *ev, uint32_t now)
{
    routing_bgp_event_t next = *ev;

    for (int round = 0; round < FEED_ROUNDS; round++) {
        routing_bgp_actions_t acts;
        followup_t f = {.connect_failed = false, .why = NULL, .lost_h = -1};

        routing_bgp_fsm_handle(&p->fsm, &next, now, &acts);
        for (uint8_t i = 0; i < acts.count; i++) {
            execute(e, p, &acts.items[i], &f);
        }
        memset(&next, 0, sizeof(next));
        if (f.connect_failed) {
            next.type = ROUTING_BGP_EV_CONNECT_FAILED;
            next.connect_failed.why = f.why;
        } else if (f.lost_h >= 0 && cur(p)->h == f.lost_h) {
            next.type = ROUTING_BGP_EV_CONN_LOST;
            next.conn_lost.incoming = false;
        } else {
            return;
        }
    }
}

static void feed_simple(routing_engine_t *e, routing_engine_peer_t *p, routing_bgp_event_type_t type, uint32_t now)
{
    routing_bgp_event_t ev;

    memset(&ev, 0, sizeof(ev));
    ev.type = type;
    feed(e, p, &ev, now);
}

static void feed_stop(routing_engine_t *e, routing_engine_peer_t *p, uint8_t subcode, uint32_t now)
{
    routing_bgp_event_t ev;

    memset(&ev, 0, sizeof(ev));
    ev.type = ROUTING_BGP_EV_STOP;
    ev.stop.subcode = subcode;
    feed(e, p, &ev, now);
    /* the FSM closed both; closing again is a no-op on an empty slot */
    close_conn(e, &p->conn[0]);
    close_conn(e, &p->conn[1]);
}

/* A connection the FSM did not close although its stream ended or broke: report it lost, then close it. */
static void end_connection(routing_engine_t *e, routing_engine_peer_t *p, int h, uint32_t now)
{
    routing_bgp_event_t ev;

    memset(&ev, 0, sizeof(ev));
    ev.type = ROUTING_BGP_EV_CONN_LOST;
    if (cur(p)->h == h) {
        ev.conn_lost.incoming = false;
        feed(e, p, &ev, now);
    } else if (pend(p)->h == h) {
        ev.conn_lost.incoming = true;
        feed(e, p, &ev, now);
    }
    for (int i = 0; i < 2; i++) {
        if (p->conn[i].h == h) {
            close_conn(e, &p->conn[i]);
        }
    }
}

/* --- BGP peers and sessions --- */

static void peer_reinit(routing_engine_t *e, routing_engine_peer_t *p)
{
    const routing_bgp_fsm_config_t cfg = {
        .local_asn = e->settings.local_asn,
        .peer_asn = p->cfg.asn,
        .local_router_id = e->router_id,
        .hold_time = e->settings.hold_time,
    };

    close_conn(e, &p->conn[0]);
    close_conn(e, &p->conn[1]);
    p->cur = 0;
    memset(&p->sent, 0, sizeof(p->sent));
    memset(&p->sent_nh, 0, sizeof(p->sent_nh));
    routing_bgp_fsm_init(&p->fsm, &cfg);
}

static void start_peers(routing_engine_t *e, uint32_t now)
{
    for (int i = 0; i < ROUTING_MAX_PEERS; i++) {
        if (e->peers[i].in_use) {
            feed_simple(e, &e->peers[i], ROUTING_BGP_EV_START, now); /* no-op for a started session */
        }
    }
}

static void bgp_start(routing_engine_t *e, uint32_t router_id, uint32_t now)
{
    e->bgp_running = true;
    e->router_id = router_id;
    e->listening = e->io->bgp_listen(e->io->ctx, true);
    for (int i = 0; i < ROUTING_MAX_PEERS; i++) {
        if (e->peers[i].in_use) {
            peer_reinit(e, &e->peers[i]);
        }
    }
    start_peers(e, now);
}

static void bgp_stop(routing_engine_t *e, uint8_t subcode, uint32_t now)
{
    if (!e->bgp_running) {
        return;
    }
    for (int i = 0; i < ROUTING_MAX_PEERS; i++) {
        if (e->peers[i].in_use) {
            feed_stop(e, &e->peers[i], subcode, now);
        }
    }
    if (e->listening) {
        e->io->bgp_listen(e->io->ctx, false);
        e->listening = false;
    }
    e->bgp_running = false;
    e->router_id = 0;
}

/* Every session again from Idle with the current settings and router ID, after Cease `subcode`. */
static void bgp_restart_sessions(routing_engine_t *e, uint8_t subcode, uint32_t now)
{
    for (int i = 0; i < ROUTING_MAX_PEERS; i++) {
        if (e->peers[i].in_use) {
            feed_stop(e, &e->peers[i], subcode, now);
            peer_reinit(e, &e->peers[i]);
        }
    }
    start_peers(e, now);
}

static uint32_t wanted_router_id(const routing_engine_t *e)
{
    if (e->state.backbone_ipv4 != 0) {
        return e->state.backbone_ipv4;
    }
    return e->settings.router_id_set ? e->settings.router_id : 0;
}

/* Starts, stops or restarts BGP as the method and the state allow. Nothing before the first poll. */
static void bgp_reconcile(routing_engine_t *e, uint32_t now)
{
    if (!e->have_state) {
        return;
    }
    uint32_t rid = wanted_router_id(e);

    e->blocked = NULL;
    if (e->settings.method == ROUTING_METHOD_BGP) {
        if (rid == 0) {
            e->blocked = ROUTING_BLOCKED_NO_ROUTER_ID;
        } else if (!e->state.have_link_local) {
            e->blocked = ROUTING_BLOCKED_NO_LINK_LOCAL;
        }
    }
    bool want = e->settings.method == ROUTING_METHOD_BGP && e->blocked == NULL;

    if (!want) {
        bgp_stop(e, e->settings.method == ROUTING_METHOD_BGP ? ROUTING_BGP_CEASE_CONFIG_CHANGE
                 : ROUTING_BGP_CEASE_ADMIN_SHUTDOWN, now);
        return;
    }
    if (!e->bgp_running) {
        bgp_start(e, rid, now);
        return;
    }
    if (rid != e->router_id) {
        e->router_id = rid;
        bgp_restart_sessions(e, ROUTING_BGP_CEASE_CONFIG_CHANGE, now);
    }
    if (!e->listening) {
        e->listening = e->io->bgp_listen(e->io->ctx, true);
    }
}

static int find_peer_setting(const routing_settings_t *s, const uint8_t addr[16])
{
    for (int i = 0; i < s->peer_count && i < ROUTING_MAX_PEERS; i++) {
        if (memcmp(s->peers[i].addr, addr, 16) == 0) {
            return i;
        }
    }
    return -1;
}

/*
 * Brings the peer slots in line with e->settings (`old` the settings before): a removed peer or one whose ASN changed
 * gets Cease 6/3, every session gets 6/6 when the local ASN or hold time changed, new peers get a slot. While BGP
 * runs the sessions are started again. The caller reconciles the router ID afterwards.
 */
static void sync_peers(routing_engine_t *e, const routing_settings_t *old, uint32_t now)
{
    const routing_settings_t *s = &e->settings;
    bool local_changed = old->local_asn != s->local_asn || old->hold_time != s->hold_time;

    for (int i = 0; i < ROUTING_MAX_PEERS; i++) {
        routing_engine_peer_t *p = &e->peers[i];
        if (!p->in_use) {
            continue;
        }
        int idx = find_peer_setting(s, p->cfg.addr);
        if (idx < 0 || s->peers[idx].asn != p->cfg.asn) {
            if (e->bgp_running) {
                feed_stop(e, p, ROUTING_BGP_CEASE_PEER_DECONFIGURED, now);
            }
            if (idx < 0) {
                peer_reinit(e, p);
                p->in_use = false;
                continue;
            }
            p->cfg = s->peers[idx];
            peer_reinit(e, p);
        } else if (local_changed) {
            if (e->bgp_running) {
                feed_stop(e, p, ROUTING_BGP_CEASE_CONFIG_CHANGE, now);
            }
            peer_reinit(e, p);
        }
    }
    for (int i = 0; i < s->peer_count && i < ROUTING_MAX_PEERS; i++) {
        if (peer_by_addr(e, s->peers[i].addr) != NULL) {
            continue;
        }
        for (int j = 0; j < ROUTING_MAX_PEERS; j++) {
            if (!e->peers[j].in_use) {
                e->peers[j].in_use = true;
                e->peers[j].cfg = s->peers[i];
                peer_reinit(e, &e->peers[j]);
                break;
            }
        }
    }
    if (e->bgp_running) {
        start_peers(e, now);
    }
}

/* --- BGP receive --- */

/* The event for one complete message `msg` of `type` (header checked) on the current or the pending connection. */
static void message_event(const routing_engine_peer_t *p, const uint8_t *msg, uint16_t len, uint8_t type,
                          bool incoming, routing_bgp_event_t *ev)
{
    routing_bgp_err_t err = {0, 0};

    memset(ev, 0, sizeof(*ev));
    switch (type) {
    case ROUTING_BGP_MSG_OPEN:
        err = routing_bgp_decode_open(msg, len, p->cfg.asn, &ev->open_rx.open);
        if (err.code == 0) {
            ev->type = ROUTING_BGP_EV_OPEN_RX;
            ev->open_rx.incoming = incoming;
            return;
        }
        break;
    case ROUTING_BGP_MSG_NOTIFICATION:
        ev->type = ROUTING_BGP_EV_NOTIFICATION_RX;
        ev->notification_rx.incoming = incoming;
        (void)routing_bgp_decode_notification(msg, len, &ev->notification_rx.code, &ev->notification_rx.subcode);
        return;
    case ROUTING_BGP_MSG_UPDATE:
    case ROUTING_BGP_MSG_KEEPALIVE:
        if (incoming) {
            /* the pending connection must open with an OPEN */
            err.code = ROUTING_BGP_ERR_FSM;
            err.subcode = 0;
            break;
        }
        if (type == ROUTING_BGP_MSG_KEEPALIVE) {
            ev->type = ROUTING_BGP_EV_KEEPALIVE_RX;
            return;
        }
        err = routing_bgp_count_update_nlri(msg, len, &ev->update_rx.nlri);
        if (err.code == 0) {
            ev->type = ROUTING_BGP_EV_UPDATE_RX;
            return;
        }
        break;
    default:
        err.code = 1;
        err.subcode = 3;
        break;
    }
    /* OPEN_BAD is the FSM's "message rejected: send this NOTIFICATION and drop the connection" */
    memset(ev, 0, sizeof(*ev));
    ev->type = ROUTING_BGP_EV_OPEN_BAD;
    ev->open_bad.err = err;
    ev->open_bad.incoming = incoming;
}

/* Hands every complete message buffered on `h` to the FSM; a partial header or message waits for more bytes. */
static void process_rx(routing_engine_t *e, routing_engine_peer_t *p, int h, uint32_t now)
{
    for (;;) {
        routing_engine_conn_t *c;
        bool incoming;

        /* the connection may have moved (adopted) or gone (closed) by the previous message */
        if (cur(p)->h == h) {
            c = cur(p);
            incoming = false;
        } else if (pend(p)->h == h) {
            c = pend(p);
            incoming = true;
        } else {
            return;
        }
        if (c->connecting || c->len < ROUTING_BGP_HDR_LEN) {
            return;
        }
        uint8_t type = 0;
        uint16_t len = 0;
        routing_bgp_event_t ev;
        routing_bgp_err_t err = routing_bgp_decode_header(c->buf, c->len, &type, &len);

        if (err.code != 0) {
            memset(&ev, 0, sizeof(ev));
            ev.type = ROUTING_BGP_EV_OPEN_BAD;
            ev.open_bad.err = err;
            ev.open_bad.incoming = incoming;
            feed(e, p, &ev, now);
            /* the stream cannot be resynchronised: make sure the connection is gone */
            end_connection(e, p, h, now);
            return;
        }
        if (len > c->len) {
            return;
        }
        message_event(p, c->buf, len, type, incoming, &ev);
        c->len -= len;
        memmove(c->buf, c->buf + len, c->len);
        feed(e, p, &ev, now);
    }
}

/* --- announcement set --- */

static void compute_set(const routing_engine_t *e, routing_prefix_set_t *out)
{
    const routing_settings_t *s = &e->settings;
    routing_announce_input_t in = {
        .method = s->method,
        .omr_mode = s->omr_mode,
        .custom = s->omr_prefix,
        .announce_ula = s->announce_ula,
        .ula_known = e->state.ula_known,
        .ula = e->state.ula,
        /* read for the custom prefix of the last poll: a new prefix counts once a poll has seen it */
        .custom_in_netdata = e->state.custom_in_netdata && prefix_equal(&e->state_custom, &s->omr_prefix),
                                     .ula_in_netdata = e->state.ula_in_netdata,
                                     .br_up = e->have_state && e->state.br_up,
    };

    routing_announce_compute(&in, out);
}

static void announce_update(routing_engine_t *e, uint32_t now)
{
    routing_prefix_set_t next;
    routing_prefix_set_t added;
    routing_prefix_set_t removed;

    compute_set(e, &next);
    routing_announce_diff(&e->announced, &next, &added, &removed);
    if (added.count == 0 && removed.count == 0) {
        return;
    }
    e->announced = next;
    if (e->bgp_running) {
        for (int i = 0; i < ROUTING_MAX_PEERS; i++) {
            if (e->peers[i].in_use) {
                feed_simple(e, &e->peers[i], ROUTING_BGP_EV_ANNOUNCE_CHANGED, now);
            }
        }
    }
    if (e->sched_on) {
        routing_ripng_sched_set_changed(&e->sched, &e->announced, now);
    }
}

static bool nexthop_equal(const routing_bgp_nexthop_t *a, const routing_bgp_nexthop_t *b)
{
    return a->has_global == b->has_global && (!a->has_global || memcmp(a->global, b->global, 16) == 0) &&
           memcmp(a->link_local, b->link_local, 16) == 0;
}

/*
 * The backbone next-hop changed (typically a global address appearing after the session came up on link-local
 * alone): every Established session announces what it announced again with the new next-hop. No reset.
 */
static void nexthop_update(routing_engine_t *e, uint32_t now)
{
    if (!e->bgp_running) {
        return;
    }
    for (int i = 0; i < ROUTING_MAX_PEERS; i++) {
        routing_engine_peer_t *p = &e->peers[i];
        if (!p->in_use || routing_bgp_fsm_state(&p->fsm) != ROUTING_BGP_ESTABLISHED ||
                nexthop_equal(&p->sent_nh, &e->state.nexthop)) {
            continue;
        }
        followup_t f = {.connect_failed = false, .why = NULL, .lost_h = -1};
        bool ibgp = p->cfg.asn == e->settings.local_asn;
        /* the codec returns 0 for an empty set: nothing was announced, nothing to correct */
        send_current(e, p, routing_bgp_encode_update(e->tx, sizeof(e->tx), ibgp, e->settings.local_asn,
                                                     &e->state.nexthop, &p->sent), &f);
        p->sent_nh = e->state.nexthop;
        if (f.lost_h >= 0 && cur(p)->h == f.lost_h) {
            routing_bgp_event_t ev;
            memset(&ev, 0, sizeof(ev));
            ev.type = ROUTING_BGP_EV_CONN_LOST;
            feed(e, p, &ev, now);
        }
    }
}

/* --- RIPng --- */

static void ripng_multicast(routing_engine_t *e, const routing_ripng_rte_t *rtes, size_t n, uint32_t now)
{
    size_t off = 0;

    while (off < n) {
        size_t consumed = 0;
        size_t len = routing_ripng_encode_response(e->ripng_tx, sizeof(e->ripng_tx), rtes + off, n - off, &consumed);
        if (len == 0) {
            return;
        }
        if (e->io->ripng_send(e->io->ctx, NULL, ROUTING_RIPNG_PORT, e->ripng_tx, len) == 0) {
            e->ripng.updates_sent++;
            e->ripng_ever = true;
            e->ripng_last_ms = now;
        }
        off += consumed;
    }
}

static void ripng_sched_stop(routing_engine_t *e, uint32_t now)
{
    routing_ripng_rte_t rtes[ROUTING_RIPNG_MAX_RTES];

    if (!e->sched_on) {
        return;
    }
    size_t n = routing_ripng_sched_stop(&e->sched, rtes, ROUTING_RIPNG_MAX_RTES);
    e->sched_on = false;
    ripng_multicast(e, rtes, n, now);
}

/* Opens the socket if needed, and runs the scheduler exactly while the socket is open and the border router up. */
static void ripng_sync(routing_engine_t *e, uint32_t now)
{
    if (!e->ripng_running) {
        return;
    }
    if (!e->ripng_open) {
        e->ripng_open = e->io->ripng_socket(e->io->ctx, true);
    }
    bool want = e->ripng_open && e->have_state && e->state.br_up;

    if (want && !e->sched_on) {
        routing_ripng_sched_start(&e->sched, &e->announced, now, e->io->rand32);
        e->sched_on = true;
    } else if (!want) {
        ripng_sched_stop(e, now);
    }
}

static void ripng_stop(routing_engine_t *e, uint32_t now)
{
    if (!e->ripng_running) {
        return;
    }
    ripng_sched_stop(e, now);
    if (e->ripng_open) {
        e->io->ripng_socket(e->io->ctx, false);
        e->ripng_open = false;
    }
    e->ripng_running = false;
}

static void ripng_unicast(routing_engine_t *e, const uint8_t dst[16], uint16_t port, size_t len, bool *sent)
{
    if (len > 0 && e->io->ripng_send(e->io->ctx, dst, port, e->ripng_tx, len) == 0) {
        *sent = true;
    }
}

/* --- settings and senders --- */

static void apply_publisher(routing_engine_t *e)
{
    e->publisher_dirty = !e->io->publisher_apply(e->io->ctx, &e->settings);
}

static void start_sender(routing_engine_t *e, uint32_t now)
{
    if (e->settings.method == ROUTING_METHOD_RIPNG) {
        e->ripng_running = true;
        ripng_sync(e, now);
    }
    bgp_reconcile(e, now);
}

static void stop_sender(routing_engine_t *e, uint32_t now)
{
    ripng_stop(e, now);
    bgp_stop(e, ROUTING_BGP_CEASE_ADMIN_SHUTDOWN, now);
}

/* --- public --- */

void routing_engine_init(routing_engine_t *e, const routing_engine_io_t *io, const routing_settings_t *s)
{
    memset(e, 0, sizeof(*e));
    e->io = io;
    e->settings = *s;
    for (int i = 0; i < ROUTING_MAX_PEERS; i++) {
        conn_reset(&e->peers[i].conn[0]);
        conn_reset(&e->peers[i].conn[1]);
    }
    for (int i = 0; i < s->peer_count && i < ROUTING_MAX_PEERS; i++) {
        e->peers[i].in_use = true;
        e->peers[i].cfg = s->peers[i];
        peer_reinit(e, &e->peers[i]);
    }
}

void routing_engine_start(routing_engine_t *e, uint32_t now_ms)
{
    if (e->started) {
        return;
    }
    e->started = true;
    start_sender(e, now_ms);
}

void routing_engine_set_settings(routing_engine_t *e, const routing_settings_t *s, uint32_t now_ms)
{
    uint32_t diff = routing_settings_diff(&e->settings, s);
    routing_settings_t old = e->settings;

    if (diff == 0) {
        return;
    }
    if (diff & ROUTING_CHANGE_METHOD) {
        stop_sender(e, now_ms);
        e->settings = *s;
        sync_peers(e, &old, now_ms); /* nothing runs: only the slots */
        apply_publisher(e);
        compute_set(e, &e->announced); /* both senders start from scratch */
        start_sender(e, now_ms);
        return;
    }
    e->settings = *s;
    if (diff & ROUTING_CHANGE_BGP_SESSIONS) {
        sync_peers(e, &old, now_ms);
        bgp_reconcile(e, now_ms); /* a configured router ID may now be the one in use */
    }
    if (diff & ROUTING_CHANGE_PREFIXES) {
        apply_publisher(e);
        announce_update(e, now_ms);
    }
}

void routing_engine_poll(routing_engine_t *e, const routing_engine_state_t *st, uint32_t now_ms)
{
    e->state = *st;
    e->state_custom = e->settings.omr_prefix;
    e->have_state = true;
    nexthop_update(e, now_ms); /* before the diff, so that added prefixes are not sent twice */
    announce_update(e, now_ms);
    ripng_sync(e, now_ms);
    if (e->publisher_dirty) {
        apply_publisher(e);
    }
    bgp_reconcile(e, now_ms);
    routing_engine_tick(e, now_ms);
}

void routing_engine_tick(routing_engine_t *e, uint32_t now_ms)
{
    for (int i = 0; i < ROUTING_MAX_PEERS; i++) {
        routing_engine_peer_t *p = &e->peers[i];
        if (!p->in_use) {
            continue;
        }
        uint32_t at = routing_bgp_fsm_next_deadline_ms(&p->fsm);
        if (at != UINT32_MAX && due(now_ms, at)) {
            feed_simple(e, p, ROUTING_BGP_EV_TICK, now_ms);
        }
    }
    if (e->sched_on) {
        routing_ripng_rte_t rtes[ROUTING_RIPNG_MAX_RTES];
        size_t n = routing_ripng_sched_poll(&e->sched, now_ms, rtes, ROUTING_RIPNG_MAX_RTES);
        ripng_multicast(e, rtes, n, now_ms);
    }
}

static void earliest(uint32_t *best, bool *found, uint32_t at)
{
    if (at != UINT32_MAX && (!*found || (int32_t)(at - *best) < 0)) {
        *best = at;
        *found = true;
    }
}

uint32_t routing_engine_next_deadline_ms(const routing_engine_t *e)
{
    uint32_t best = UINT32_MAX;
    bool found = false;

    for (int i = 0; i < ROUTING_MAX_PEERS; i++) {
        if (e->peers[i].in_use) {
            earliest(&best, &found, routing_bgp_fsm_next_deadline_ms(&e->peers[i].fsm));
        }
    }
    if (e->sched_on) {
        earliest(&best, &found, routing_ripng_sched_next_deadline_ms(&e->sched));
    }
    return best;
}

void routing_engine_tcp_accepted(routing_engine_t *e, int h, const uint8_t addr[16], uint32_t now_ms)
{
    routing_engine_peer_t *p = e->bgp_running ? peer_by_addr(e, addr) : NULL;
    routing_bgp_event_t ev;

    if (p == NULL || (cur(p)->h >= 0 && pend(p)->h >= 0)) {
        e->io->tcp_close(e->io->ctx, h);
        return;
    }
    memset(&ev, 0, sizeof(ev));
    if (cur(p)->h >= 0) {
        /* held as the pending connection; the FSM adopts it, rejects it or waits for its OPEN */
        pend(p)->h = h;
        ev.type = ROUTING_BGP_EV_INCOMING_WHILE_BUSY;
    } else {
        cur(p)->h = h;
        ev.type = ROUTING_BGP_EV_CONNECTED;
        ev.connected.incoming = true;
    }
    feed(e, p, &ev, now_ms);
}

void routing_engine_tcp_connected(routing_engine_t *e, int h, bool ok, const char *why, uint32_t now_ms)
{
    routing_engine_peer_t *p = peer_by_handle(e, h);
    routing_bgp_event_t ev;

    if (p == NULL || cur(p)->h != h || !cur(p)->connecting) {
        return;
    }
    memset(&ev, 0, sizeof(ev));
    if (ok) {
        cur(p)->connecting = false;
        ev.type = ROUTING_BGP_EV_CONNECTED;
        ev.connected.incoming = false;
        feed(e, p, &ev, now_ms);
        return;
    }
    ev.type = ROUTING_BGP_EV_CONNECT_FAILED;
    ev.connect_failed.why = why;
    feed(e, p, &ev, now_ms);
    if (cur(p)->h == h) {
        close_conn(e, cur(p));
    }
}

size_t routing_engine_tcp_rx_space(routing_engine_t *e, int h, uint8_t **buf)
{
    routing_engine_peer_t *p = peer_by_handle(e, h);

    *buf = NULL;
    if (p == NULL) {
        return 0;
    }
    routing_engine_conn_t *c = p->conn[0].h == h ? &p->conn[0] : &p->conn[1];
    if (c->connecting) {
        return 0;
    }
    *buf = c->buf + c->len;
    return sizeof(c->buf) - c->len;
}

void routing_engine_tcp_rx(routing_engine_t *e, int h, int n, uint32_t now_ms)
{
    routing_engine_peer_t *p = peer_by_handle(e, h);

    if (p == NULL) {
        return;
    }
    routing_engine_conn_t *c = p->conn[0].h == h ? &p->conn[0] : &p->conn[1];
    if (n <= 0) {
        end_connection(e, p, h, now_ms);
        return;
    }
    size_t room = sizeof(c->buf) - c->len;
    c->len += (uint16_t)((size_t)n < room ? (size_t)n : room);
    process_rx(e, p, h, now_ms);
}

void routing_engine_ripng_rx(routing_engine_t *e, const uint8_t src[16], uint16_t port, const uint8_t *buf,
                             size_t len, uint32_t now_ms)
{
    routing_ripng_rte_t rtes[ROUTING_RIPNG_MAX_RTES];
    bool sent = false;

    if (!e->ripng_running || !e->ripng_open) {
        return;
    }
    switch (routing_ripng_classify(buf, len)) {
    case ROUTING_RIPNG_INVALID:
    case ROUTING_RIPNG_RESPONSE:
        e->ripng.responses_dropped++;
        return;
    case ROUTING_RIPNG_REQUEST_ALL: {
        size_t n = routing_ripng_table(&e->announced, rtes, ROUTING_RIPNG_MAX_RTES);
        size_t off = 0;
        while (off < n) {
            size_t consumed = 0;
            size_t out = routing_ripng_encode_response(e->ripng_tx, sizeof(e->ripng_tx), rtes + off, n - off,
                                                       &consumed);
            if (out == 0) {
                break;
            }
            ripng_unicast(e, src, port, out, &sent);
            off += consumed;
        }
        break;
    }
    case ROUTING_RIPNG_REQUEST_SOME:
        /* answered from its own buffer: encode_answer copies the request first */
        ripng_unicast(e, src, port,
                      routing_ripng_encode_answer(buf, len, &e->announced, e->ripng_tx, sizeof(e->ripng_tx)), &sent);
        break;
    }
    if (sent) {
        e->ripng.requests_answered++;
    }
}

const routing_settings_t *routing_engine_settings(const routing_engine_t *e)
{
    return &e->settings;
}

void routing_engine_status(const routing_engine_t *e, uint32_t now_ms, routing_status_t *out)
{
    const routing_settings_t *s = &e->settings;

    memset(out, 0, sizeof(*out));
    if (s->omr_mode == ROUTING_OMR_CUSTOM) {
        out->omr_prefix_known = true;
        out->omr_prefix = s->omr_prefix;
    } else if (e->have_state && e->state.pd_known) {
        out->omr_prefix_known = true;
        out->omr_prefix = e->state.pd_prefix;
    } else if (e->have_state && e->state.ula_known) {
        out->omr_prefix_known = true;
        out->omr_prefix = e->state.ula;
    }
    out->ula_prefix_known = e->have_state && e->state.ula_known;
    if (out->ula_prefix_known) {
        out->ula_prefix = e->state.ula;
    }
    out->announced = e->announced;
    if (s->method == ROUTING_METHOD_BGP) {
        out->bgp_router_id = e->bgp_running ? e->router_id : 0;
        out->bgp_blocked = e->blocked;
    }
    for (int i = 0; i < s->peer_count && i < ROUTING_MAX_PEERS; i++) {
        const routing_engine_peer_t *p = peer_by_addr_const(e, s->peers[i].addr);
        if (p == NULL) {
            continue;
        }
        routing_bgp_peer_status_t *ps = &out->peers[i];
        ps->state = routing_bgp_fsm_state(&p->fsm);
        ps->uptime_s = routing_bgp_fsm_uptime_s(&p->fsm, now_ms);
        snprintf(ps->last_error, sizeof(ps->last_error), "%s", routing_bgp_fsm_last_error(&p->fsm));
        ps->received_dropped = routing_bgp_fsm_received_dropped(&p->fsm);
        ps->resets = routing_bgp_fsm_resets(&p->fsm);
    }
    out->ripng = e->ripng;
    out->ripng.last_update_ms_ago = e->ripng_ever ? (int64_t)(uint32_t)(now_ms - e->ripng_last_ms) : -1;
}

void routing_engine_metrics_view(const routing_engine_t *e, uint32_t now_ms, routing_metrics_view_t *out)
{
    routing_status_t st;
    const routing_settings_t *s = &e->settings;

    routing_engine_status(e, now_ms, &st);
    memset(out, 0, sizeof(*out));
    out->method = s->method;
    out->announced_count = st.announced.count;
    out->omr_custom = s->omr_mode == ROUTING_OMR_CUSTOM;
    if (s->method == ROUTING_METHOD_BGP) {
        out->peer_count = s->peer_count;
        for (int i = 0; i < s->peer_count && i < ROUTING_MAX_PEERS; i++) {
            memcpy(out->peers[i].addr, s->peers[i].addr, 16);
            out->peers[i].state = st.peers[i].state;
            out->peers[i].uptime_s = st.peers[i].uptime_s;
            out->peers[i].received_dropped = st.peers[i].received_dropped;
            out->peers[i].resets = st.peers[i].resets;
        }
    }
    out->ripng_updates_sent = st.ripng.updates_sent;
    out->ripng_requests_answered = st.ripng.requests_answered;
    out->ripng_responses_dropped = st.ripng.responses_dropped;
}
