#pragma once

// BGP session state machine (spec section 5), one instance per configured peer: the RFC 4271 section 8 states Idle,
// Connect, Active, OpenSent, OpenConfirm and Established, the connect-retry, hold and keepalive timers, and connection
// collision resolution (section 6.8). Pure and event-driven: the routing task feeds it socket events and its clock and
// carries out the actions it returns with the codec (bgp_msg.h). It makes no socket or codec calls. Host-tested.
//
// Connections. The FSM has at most one *current* connection (or outgoing attempt), which every SEND_* and CLOSE
// action refers to, and at most one *pending incoming* connection: one the task reported with INCOMING_WHILE_BUSY in
// OpenSent or OpenConfirm, held until its OPEN decides the collision. The task reads that OPEN and reports it with
// OPEN_RX (or OPEN_BAD) with `incoming` set; it sends nothing on that connection before ADOPT_INCOMING. While a
// connection is pending the task refuses further incoming connections for this peer itself and does not report them.
// The `incoming` flags of OPEN_RX, OPEN_BAD, CONN_LOST and NOTIFICATION_RX are true for events on the pending
// connection and false for the current one; without a pending connection they are ignored.
//
// Timers. Times are milliseconds on a free-running uint32_t clock; comparisons are wrap-safe. Connect timeout 10 s;
// OpenSent hold timer 240 s; after the OPEN exchange the hold time is the smaller of the two offered and keepalives go
// every hold / 3; with a hold time of 0 neither runs. The connect-retry backoff starts at 5 s and doubles after every
// scheduled retry up to 120 s; it is reset to 5 s on reaching Established, so a drop from Established retries after
// 5 s. Retries are scheduled after a failed connect (moving to Active) and after a drop (moving to Idle); STOP schedules
// nothing until the next START.

#include <stdbool.h>
#include <stdint.h>

#include "bgp_msg.h"
#include "routing_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ROUTING_BGP_FSM_MAX_ACTIONS 4
#define ROUTING_BGP_CONNECT_TIMEOUT_MS 10000u
#define ROUTING_BGP_OPENSENT_HOLD_S 240u
#define ROUTING_BGP_BACKOFF_MIN_MS 5000u
#define ROUTING_BGP_BACKOFF_MAX_MS 120000u

/** NOTIFICATION codes and subcodes the FSM sends (RFC 4271 section 4.5, RFC 4486, RFC 6608). */
#define ROUTING_BGP_ERR_HOLD_TIMER 4
#define ROUTING_BGP_ERR_FSM 5
#define ROUTING_BGP_ERR_FSM_IN_OPENSENT 1
#define ROUTING_BGP_ERR_FSM_IN_OPENCONFIRM 2
#define ROUTING_BGP_ERR_FSM_IN_ESTABLISHED 3
#define ROUTING_BGP_ERR_CEASE 6
#define ROUTING_BGP_CEASE_ADMIN_SHUTDOWN 2
#define ROUTING_BGP_CEASE_PEER_DECONFIGURED 3
#define ROUTING_BGP_CEASE_CONNECTION_REJECTED 5
#define ROUTING_BGP_CEASE_COLLISION 7

typedef struct {
    uint32_t local_asn;
    uint32_t peer_asn;
    uint32_t local_router_id; /**< host order */
    uint16_t hold_time;       /**< offered in OPEN: 0 or 3..65535 */
} routing_bgp_fsm_config_t;

typedef enum {
    ROUTING_BGP_EV_START,               /**< the peer is configured and BGP may run */
    ROUTING_BGP_EV_STOP,                /**< method changed away from bgp (6/2) or peer removed (6/3) */
    ROUTING_BGP_EV_CONNECTED,           /**< a connection is up and is now the current one */
    ROUTING_BGP_EV_CONNECT_FAILED,      /**< the outgoing attempt failed */
    ROUTING_BGP_EV_CONN_LOST,           /**< TCP reset or EOF */
    ROUTING_BGP_EV_OPEN_RX,             /**< a valid OPEN (routing_bgp_decode_open returned OK) */
    ROUTING_BGP_EV_OPEN_BAD,            /**< an OPEN the codec rejected */
    ROUTING_BGP_EV_KEEPALIVE_RX,
    ROUTING_BGP_EV_UPDATE_RX,           /**< a valid UPDATE; its NLRI counted by routing_bgp_count_update_nlri */
    ROUTING_BGP_EV_NOTIFICATION_RX,
    ROUTING_BGP_EV_INCOMING_WHILE_BUSY, /**< an incoming connection accepted while the FSM has a current one */
    ROUTING_BGP_EV_ANNOUNCE_CHANGED,    /**< the announcement set changed */
    ROUTING_BGP_EV_TICK,                /**< the clock passed next_deadline_ms (extra ticks are harmless) */
} routing_bgp_event_type_t;

/** The task sends *_RX events for complete messages only; a short read is never an event. */
typedef struct {
    routing_bgp_event_type_t type;
    union {
        struct {
            uint8_t subcode; /**< Cease subcode sent when connected */
        } stop;
        struct {
            bool incoming; /**< accepted rather than connected out */
        } connected;
        struct {
            const char *why; /**< for last_error, e.g. "refused"; NULL allowed */
        } connect_failed;
        struct {
            bool incoming;
        } conn_lost;
        struct {
            routing_bgp_open_t open;
            bool incoming;
        } open_rx;
        struct {
            routing_bgp_err_t err; /**< the NOTIFICATION to send */
            bool incoming;
        } open_bad;
        struct {
            uint32_t nlri;
        } update_rx;
        struct {
            uint8_t code;
            uint8_t subcode;
            bool incoming;
        } notification_rx;
    };
} routing_bgp_event_t;

typedef enum {
    ROUTING_BGP_ACT_CONNECT,           /**< open an outgoing connection (the task gives up after 10 s, see TICK) */
    ROUTING_BGP_ACT_SEND_OPEN,         /**< OPEN with the configured ASN, hold time and router ID */
    ROUTING_BGP_ACT_SEND_KEEPALIVE,
    ROUTING_BGP_ACT_SEND_NOTIFICATION, /**< `notification` */
    /**
     * UPDATE announcing the whole current announcement set. Emitted whether or not the set is empty; the task skips
     * the send when the codec returns 0 for an empty set.
     */
    ROUTING_BGP_ACT_SEND_FULL_SET,
    /**
     * One UPDATE withdrawing the removed prefixes and one announcing the added ones, against what this session last
     * sent. Emitted whether or not the diff is empty; the task skips each send the codec returns 0 for.
     */
    ROUTING_BGP_ACT_SEND_DIFF,
    ROUTING_BGP_ACT_CLOSE,             /**< close the current connection or outgoing attempt (no-op without one) */
    /**
     * Close the current connection or outgoing attempt, if any; the incoming connection (the pending one, or the one
     * INCOMING_WHILE_BUSY just reported) becomes current.
     */
    ROUTING_BGP_ACT_ADOPT_INCOMING,
    /**
     * Send `notification` on the incoming connection (pending, or just reported) and close it; with code 0 (the
     * connection was lost or sent a NOTIFICATION) close it without sending anything.
     */
    ROUTING_BGP_ACT_REJECT_INCOMING,
} routing_bgp_action_type_t;

typedef struct {
    routing_bgp_action_type_t type;
    routing_bgp_err_t notification; /**< SEND_NOTIFICATION and REJECT_INCOMING only */
} routing_bgp_action_t;

/** Carried out in order. */
typedef struct {
    routing_bgp_action_t items[ROUTING_BGP_FSM_MAX_ACTIONS];
    uint8_t count;
} routing_bgp_actions_t;

/** One peer's session. Fields are private; use the functions below. */
typedef struct {
    routing_bgp_fsm_config_t cfg;
    routing_bgp_state_t state;
    bool started;
    bool pending_incoming;
    uint16_t hold_time; /**< negotiated, valid from OpenConfirm on */
    uint32_t backoff_ms;
    bool retry_armed;
    uint32_t retry_at;
    uint32_t connect_at;
    bool hold_armed;
    uint32_t hold_at;
    bool keepalive_armed;
    uint32_t keepalive_at;
    uint32_t established_at;
    char last_error[ROUTING_LAST_ERROR_LEN];
    uint32_t received_dropped;
    uint32_t resets;
} routing_bgp_fsm_t;

/** Idle, not started, backoff 5 s, counters zero, last_error "". */
void routing_bgp_fsm_init(routing_bgp_fsm_t *f, const routing_bgp_fsm_config_t *cfg);

/** Feeds one event at time `now_ms`; `out` is overwritten with the actions to carry out. */
void routing_bgp_fsm_handle(routing_bgp_fsm_t *f, const routing_bgp_event_t *ev, uint32_t now_ms,
                            routing_bgp_actions_t *out);

/** The earliest armed timer (send a TICK when the clock reaches it), or UINT32_MAX when none runs. */
uint32_t routing_bgp_fsm_next_deadline_ms(const routing_bgp_fsm_t *f);

routing_bgp_state_t routing_bgp_fsm_state(const routing_bgp_fsm_t *f);

/** The hold time negotiated by the last OPEN exchange (0: no keepalives and no hold timer). */
uint16_t routing_bgp_fsm_hold_time(const routing_bgp_fsm_t *f);

/** Seconds since the session reached Established; 0 when not Established. */
uint32_t routing_bgp_fsm_uptime_s(const routing_bgp_fsm_t *f, uint32_t now_ms);

/**
 * The last failure, "" when none: "hold timer expired", "notification <c>/<s> received", "notification <c>/<s> sent",
 * "connect: <why>" or "connection lost". STOP leaves it unchanged.
 */
const char *routing_bgp_fsm_last_error(const routing_bgp_fsm_t *f);

/** NLRI received in UPDATEs and dropped, over the peer's life (kept across reconnects). */
uint32_t routing_bgp_fsm_received_dropped(const routing_bgp_fsm_t *f);

/** Drops from Established (connection lost, NOTIFICATION either way, hold expiry); STOP is not counted. */
uint32_t routing_bgp_fsm_resets(const routing_bgp_fsm_t *f);

#ifdef __cplusplus
}
#endif
