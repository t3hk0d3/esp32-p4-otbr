#include "bgp_fsm.h"

#include <stdio.h>
#include <string.h>

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

static void push(routing_bgp_actions_t *out, routing_bgp_action_type_t type, uint8_t code, uint8_t subcode)
{
    /* No transition emits more than ROUTING_BGP_FSM_MAX_ACTIONS; the host tests check every list. */
    if (out->count >= ROUTING_BGP_FSM_MAX_ACTIONS) {
        return;
    }
    out->items[out->count].type = type;
    out->items[out->count].notification.code = code;
    out->items[out->count].notification.subcode = subcode;
    out->count++;
}

static bool is_connected(routing_bgp_state_t state)
{
    return state == ROUTING_BGP_OPENSENT || state == ROUTING_BGP_OPENCONFIRM || state == ROUTING_BGP_ESTABLISHED;
}

static void disarm_timers(routing_bgp_fsm_t *f)
{
    f->retry_armed = false;
    f->hold_armed = false;
    f->keepalive_armed = false;
}

static void schedule_retry(routing_bgp_fsm_t *f, uint32_t now)
{
    f->retry_armed = true;
    f->retry_at = deadline(now, f->backoff_ms);
    f->backoff_ms *= 2;
    if (f->backoff_ms > ROUTING_BGP_BACKOFF_MAX_MS) {
        f->backoff_ms = ROUTING_BGP_BACKOFF_MAX_MS;
    }
}

static uint32_t hold_ms(const routing_bgp_fsm_t *f)
{
    return (uint32_t)f->hold_time * 1000u;
}

static void restart_hold(routing_bgp_fsm_t *f, uint32_t now)
{
    /* with hold time 0 this ends the handshake guard of OpenConfirm (see accept_open) */
    f->hold_armed = f->hold_time != 0;
    f->hold_at = deadline(now, hold_ms(f));
}

static void enter_connect(routing_bgp_fsm_t *f, uint32_t now, routing_bgp_actions_t *out)
{
    push(out, ROUTING_BGP_ACT_CONNECT, 0, 0);
    f->state = ROUTING_BGP_CONNECT;
    disarm_timers(f);
    f->connect_at = deadline(now, ROUTING_BGP_CONNECT_TIMEOUT_MS);
    f->current_incoming = false;
}

/* `incoming`: the new current connection is an incoming one; `adopt`: the task must first make it current. */
static void enter_opensent(routing_bgp_fsm_t *f, uint32_t now, bool incoming, bool adopt, routing_bgp_actions_t *out)
{
    if (adopt) {
        push(out, ROUTING_BGP_ACT_ADOPT_INCOMING, 0, 0);
    }
    f->current_incoming = incoming;
    push(out, ROUTING_BGP_ACT_SEND_OPEN, 0, 0);
    f->state = ROUTING_BGP_OPENSENT;
    disarm_timers(f);
    f->hold_armed = true;
    f->hold_at = deadline(now, ROUTING_BGP_OPENSENT_HOLD_S * 1000u);
}

/* A valid OPEN on the current connection in OpenSent. */
static void accept_open(routing_bgp_fsm_t *f, const routing_bgp_open_t *open, uint32_t now,
                        routing_bgp_actions_t *out)
{
    f->hold_time = open->hold_time < f->cfg.hold_time ? open->hold_time : f->cfg.hold_time;
    push(out, ROUTING_BGP_ACT_SEND_KEEPALIVE, 0, 0);
    f->state = ROUTING_BGP_OPENCONFIRM;
    disarm_timers(f);
    f->hold_armed = true;
    if (f->hold_time != 0) {
        f->hold_at = deadline(now, hold_ms(f));
        f->keepalive_armed = true;
        f->keepalive_at = deadline(now, hold_ms(f) / 3u);
    } else {
        /* no hold timer once Established, but a peer that never confirms must not keep OpenConfirm forever */
        f->hold_at = deadline(now, ROUTING_BGP_OPENSENT_HOLD_S * 1000u);
    }
}

static void reject_pending(routing_bgp_fsm_t *f, uint8_t code, uint8_t subcode, routing_bgp_actions_t *out)
{
    push(out, ROUTING_BGP_ACT_REJECT_INCOMING, code, subcode);
    f->pending_incoming = false;
}

/*
 * The current connection ends (after an optional NOTIFICATION). A pending incoming connection takes over in OpenSent;
 * otherwise the FSM goes to Idle and schedules a retry. The caller records last_error.
 */
static void drop(routing_bgp_fsm_t *f, uint32_t now, bool notify, uint8_t code, uint8_t subcode,
                 routing_bgp_actions_t *out)
{
    if (notify) {
        push(out, ROUTING_BGP_ACT_SEND_NOTIFICATION, code, subcode);
    }
    if (f->state == ROUTING_BGP_ESTABLISHED) {
        f->resets++;
    }
    if (f->pending_incoming) {
        f->pending_incoming = false;
        enter_opensent(f, now, true, true, out);
        return;
    }
    push(out, ROUTING_BGP_ACT_CLOSE, 0, 0);
    f->state = ROUTING_BGP_IDLE;
    disarm_timers(f);
    schedule_retry(f, now);
}

static void drop_sending(routing_bgp_fsm_t *f, uint32_t now, uint8_t code, uint8_t subcode,
                         routing_bgp_actions_t *out)
{
    snprintf(f->last_error, sizeof(f->last_error), "notification %u/%u sent", code, subcode);
    drop(f, now, true, code, subcode, out);
}

/* A message the state does not allow: NOTIFICATION 5/<state> (RFC 6608). */
static void fsm_error(routing_bgp_fsm_t *f, uint32_t now, routing_bgp_actions_t *out)
{
    uint8_t subcode = f->state == ROUTING_BGP_OPENSENT ? ROUTING_BGP_ERR_FSM_IN_OPENSENT
                      : f->state == ROUTING_BGP_OPENCONFIRM ? ROUTING_BGP_ERR_FSM_IN_OPENCONFIRM
                      : ROUTING_BGP_ERR_FSM_IN_ESTABLISHED;

    drop_sending(f, now, ROUTING_BGP_ERR_FSM, subcode, out);
}

static void connect_failed(routing_bgp_fsm_t *f, uint32_t now, const char *why, routing_bgp_actions_t *out)
{
    snprintf(f->last_error, sizeof(f->last_error), "connect: %s", why != NULL ? why : "failed");
    push(out, ROUTING_BGP_ACT_CLOSE, 0, 0);
    f->state = ROUTING_BGP_ACTIVE;
    disarm_timers(f);
    schedule_retry(f, now);
}

/* RFC 4271 section 6.8: the connection opened by the side with the higher BGP identifier stays. */
static void resolve_collision(routing_bgp_fsm_t *f, const routing_bgp_open_t *open, uint32_t now,
                              routing_bgp_actions_t *out)
{
    if (f->cfg.local_router_id > open->router_id) {
        reject_pending(f, ROUTING_BGP_ERR_CEASE, ROUTING_BGP_CEASE_COLLISION, out);
        return;
    }
    push(out, ROUTING_BGP_ACT_SEND_NOTIFICATION, ROUTING_BGP_ERR_CEASE, ROUTING_BGP_CEASE_COLLISION);
    f->pending_incoming = false;
    enter_opensent(f, now, true, true, out);
    accept_open(f, open, now, out);
}

static void on_stop(routing_bgp_fsm_t *f, uint8_t subcode, routing_bgp_actions_t *out)
{
    if (is_connected(f->state)) {
        push(out, ROUTING_BGP_ACT_SEND_NOTIFICATION, ROUTING_BGP_ERR_CEASE, subcode);
        push(out, ROUTING_BGP_ACT_CLOSE, 0, 0);
    } else if (f->state == ROUTING_BGP_CONNECT || f->state == ROUTING_BGP_ACTIVE) {
        push(out, ROUTING_BGP_ACT_CLOSE, 0, 0);
    }
    if (f->pending_incoming) {
        reject_pending(f, ROUTING_BGP_ERR_CEASE, subcode, out);
    }
    f->started = false;
    f->state = ROUTING_BGP_IDLE;
    disarm_timers(f);
}

static void on_incoming_while_busy(routing_bgp_fsm_t *f, uint32_t now, routing_bgp_actions_t *out)
{
    switch (f->state) {
    case ROUTING_BGP_IDLE:
        if (f->started) {
            enter_opensent(f, now, true, true, out);
        } else {
            push(out, ROUTING_BGP_ACT_REJECT_INCOMING, ROUTING_BGP_ERR_CEASE, ROUTING_BGP_CEASE_CONNECTION_REJECTED);
        }
        break;
    case ROUTING_BGP_CONNECT:
    case ROUTING_BGP_ACTIVE:
        enter_opensent(f, now, true, true, out);
        break;
    case ROUTING_BGP_OPENSENT:
    case ROUTING_BGP_OPENCONFIRM:
    case ROUTING_BGP_ESTABLISHED:
        if (f->current_incoming) {
            /* the peer opened the current connection too: a new one means it abandoned that one */
            if (f->state == ROUTING_BGP_ESTABLISHED) {
                f->resets++;
                snprintf(f->last_error, sizeof(f->last_error), "connection lost");
            }
            enter_opensent(f, now, true, true, out);
        } else if (f->state == ROUTING_BGP_ESTABLISHED) {
            push(out, ROUTING_BGP_ACT_REJECT_INCOMING, ROUTING_BGP_ERR_CEASE, ROUTING_BGP_CEASE_COLLISION);
        } else {
            f->pending_incoming = true; /* decided when its OPEN arrives */
        }
        break;
    }
}

static void on_keepalive(routing_bgp_fsm_t *f, uint32_t now, routing_bgp_actions_t *out)
{
    switch (f->state) {
    case ROUTING_BGP_OPENSENT:
        fsm_error(f, now, out);
        break;
    case ROUTING_BGP_OPENCONFIRM:
        f->state = ROUTING_BGP_ESTABLISHED;
        f->established_at = now;
        f->backoff_ms = ROUTING_BGP_BACKOFF_MIN_MS;
        restart_hold(f, now);
        push(out, ROUTING_BGP_ACT_SEND_FULL_SET, 0, 0);
        if (f->pending_incoming) {
            reject_pending(f, ROUTING_BGP_ERR_CEASE, ROUTING_BGP_CEASE_COLLISION, out);
        }
        break;
    case ROUTING_BGP_ESTABLISHED:
        restart_hold(f, now);
        break;
    default:
        break;
    }
}

static void on_tick(routing_bgp_fsm_t *f, uint32_t now, routing_bgp_actions_t *out)
{
    switch (f->state) {
    case ROUTING_BGP_IDLE:
    case ROUTING_BGP_ACTIVE:
        if (f->retry_armed && due(now, f->retry_at)) {
            enter_connect(f, now, out);
        }
        break;
    case ROUTING_BGP_CONNECT:
        if (due(now, f->connect_at)) {
            connect_failed(f, now, "timed out", out);
        }
        break;
    case ROUTING_BGP_OPENSENT:
    case ROUTING_BGP_OPENCONFIRM:
    case ROUTING_BGP_ESTABLISHED:
        if (f->hold_armed && due(now, f->hold_at)) {
            snprintf(f->last_error, sizeof(f->last_error), "hold timer expired");
            drop(f, now, true, ROUTING_BGP_ERR_HOLD_TIMER, 0, out);
        } else if (f->keepalive_armed && due(now, f->keepalive_at)) {
            push(out, ROUTING_BGP_ACT_SEND_KEEPALIVE, 0, 0);
            f->keepalive_at = deadline(now, hold_ms(f) / 3u);
        }
        break;
    }
}

void routing_bgp_fsm_init(routing_bgp_fsm_t *f, const routing_bgp_fsm_config_t *cfg)
{
    memset(f, 0, sizeof(*f));
    f->cfg = *cfg;
    f->state = ROUTING_BGP_IDLE;
    f->backoff_ms = ROUTING_BGP_BACKOFF_MIN_MS;
}

void routing_bgp_fsm_handle(routing_bgp_fsm_t *f, const routing_bgp_event_t *ev, uint32_t now_ms,
                            routing_bgp_actions_t *out)
{
    out->count = 0;
    switch (ev->type) {
    case ROUTING_BGP_EV_START:
        if (!f->started) {
            f->started = true;
            f->backoff_ms = ROUTING_BGP_BACKOFF_MIN_MS;
            enter_connect(f, now_ms, out);
        }
        break;
    case ROUTING_BGP_EV_STOP:
        on_stop(f, ev->stop.subcode, out);
        break;
    case ROUTING_BGP_EV_CONNECTED:
        if (f->state == ROUTING_BGP_CONNECT || f->state == ROUTING_BGP_ACTIVE ||
                (f->state == ROUTING_BGP_IDLE && f->started)) {
            enter_opensent(f, now_ms, ev->connected.incoming, false, out);
        } else if (f->state == ROUTING_BGP_IDLE) {
            push(out, ROUTING_BGP_ACT_CLOSE, 0, 0);
        }
        break;
    case ROUTING_BGP_EV_CONNECT_FAILED:
        if (f->state == ROUTING_BGP_CONNECT) {
            connect_failed(f, now_ms, ev->connect_failed.why, out);
        }
        break;
    case ROUTING_BGP_EV_CONN_LOST:
        if (ev->conn_lost.incoming) {
            if (f->pending_incoming) {
                reject_pending(f, 0, 0, out);
            }
        } else if (is_connected(f->state)) {
            snprintf(f->last_error, sizeof(f->last_error), "connection lost");
            drop(f, now_ms, false, 0, 0, out);
        }
        break;
    case ROUTING_BGP_EV_OPEN_RX:
        if (ev->open_rx.incoming) {
            if (f->pending_incoming) {
                resolve_collision(f, &ev->open_rx.open, now_ms, out);
            }
        } else if (f->state == ROUTING_BGP_OPENSENT) {
            accept_open(f, &ev->open_rx.open, now_ms, out);
        } else if (is_connected(f->state)) {
            fsm_error(f, now_ms, out);
        }
        break;
    case ROUTING_BGP_EV_OPEN_BAD:
        if (ev->open_bad.incoming) {
            if (f->pending_incoming) {
                reject_pending(f, ev->open_bad.err.code, ev->open_bad.err.subcode, out);
            }
        } else if (is_connected(f->state)) {
            drop_sending(f, now_ms, ev->open_bad.err.code, ev->open_bad.err.subcode, out);
        }
        break;
    case ROUTING_BGP_EV_KEEPALIVE_RX:
        on_keepalive(f, now_ms, out);
        break;
    case ROUTING_BGP_EV_UPDATE_RX:
        if (f->state == ROUTING_BGP_ESTABLISHED) {
            f->received_dropped += ev->update_rx.nlri;
            restart_hold(f, now_ms);
        } else if (is_connected(f->state)) {
            fsm_error(f, now_ms, out);
        }
        break;
    case ROUTING_BGP_EV_NOTIFICATION_RX:
        if (ev->notification_rx.incoming) {
            if (f->pending_incoming) {
                reject_pending(f, 0, 0, out);
            }
        } else if (is_connected(f->state)) {
            snprintf(f->last_error, sizeof(f->last_error), "notification %u/%u received", ev->notification_rx.code,
                     ev->notification_rx.subcode);
            drop(f, now_ms, false, 0, 0, out);
        }
        break;
    case ROUTING_BGP_EV_INCOMING_WHILE_BUSY:
        on_incoming_while_busy(f, now_ms, out);
        break;
    case ROUTING_BGP_EV_ANNOUNCE_CHANGED:
        if (f->state == ROUTING_BGP_ESTABLISHED) {
            push(out, ROUTING_BGP_ACT_SEND_DIFF, 0, 0);
        }
        break;
    case ROUTING_BGP_EV_TICK:
        on_tick(f, now_ms, out);
        break;
    }
}

static void earliest(uint32_t *best, bool *found, bool armed, uint32_t at)
{
    if (armed && (!*found || (int32_t)(at - *best) < 0)) {
        *best = at;
        *found = true;
    }
}

uint32_t routing_bgp_fsm_next_deadline_ms(const routing_bgp_fsm_t *f)
{
    uint32_t best = UINT32_MAX;
    bool found = false;

    earliest(&best, &found, f->retry_armed, f->retry_at);
    earliest(&best, &found, f->state == ROUTING_BGP_CONNECT, f->connect_at);
    earliest(&best, &found, f->hold_armed, f->hold_at);
    earliest(&best, &found, f->keepalive_armed, f->keepalive_at);
    return best;
}

routing_bgp_state_t routing_bgp_fsm_state(const routing_bgp_fsm_t *f)
{
    return f->state;
}

uint16_t routing_bgp_fsm_hold_time(const routing_bgp_fsm_t *f)
{
    return f->hold_time;
}

uint32_t routing_bgp_fsm_uptime_s(const routing_bgp_fsm_t *f, uint32_t now_ms)
{
    return f->state == ROUTING_BGP_ESTABLISHED ? (now_ms - f->established_at) / 1000u : 0;
}

const char *routing_bgp_fsm_last_error(const routing_bgp_fsm_t *f)
{
    return f->last_error;
}

uint32_t routing_bgp_fsm_received_dropped(const routing_bgp_fsm_t *f)
{
    return f->received_dropped;
}

uint32_t routing_bgp_fsm_resets(const routing_bgp_fsm_t *f)
{
    return f->resets;
}
