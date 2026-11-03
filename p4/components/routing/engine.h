#pragma once

// Routing engine (spec section 5, "Routing task"): every decision of the routing task, pure and host-tested. The task
// (task.c) owns the sockets, the clock and the OpenThread lock and calls in here; the engine drives the announcement
// set, the BGP sessions (bgp_fsm.h + bgp_msg.h) and the RIPng sender (ripng.h), and acts through routing_engine_io_t.
// It allocates nothing: every buffer is in routing_engine_t.
//
// Clock: uint32_t milliseconds, compared wrap-safe, passed to every call.
//
// Settings changes (routing_engine_set_settings), by routing_settings_diff:
// - METHOD: the old sender stops completely (BGP: Cease 6/2 on every session, sockets closed, listener closed; RIPng:
//   every prefix at metric 16 once, socket closed), then the publisher applies the new settings (DHCPv6-PD follows
//   the method), then the new sender starts;
// - BGP_SESSIONS: a removed peer, or one whose ASN changed, gets Cease 6/3 (Peer De-configured); a change of the local
//   ASN, hold time or router ID in use restarts every session with Cease 6/6 (Other Configuration Change); a new peer
//   is started. Unchanged sessions are left alone;
// - PREFIXES: the publisher applies the new settings and the announcement set is recomputed; no session is reset.
// The publisher is applied only on settings changes (and retried at the next poll when the apply failed), never on a
// session or socket event.
//
// BGP runs while the method is bgp and the state has a router ID (the backbone IPv4 address, else the configured one)
// and a backbone link-local address (the next-hop); otherwise routing_status_t.bgp_blocked says why. Sessions start at
// the first routing_engine_poll, not before, so that the router ID is known.
//
// TCP. The task reports connections by handle (a socket descriptor). Per peer the engine keeps a current connection
// (or outgoing attempt) and at most one pending incoming connection, each with a ROUTING_BGP_MAX_MSG receive buffer.
// It frames the byte stream into messages: a partial header or message waits for more bytes and is never an error;
// a complete header the codec rejects ends the connection with that NOTIFICATION. An accepted connection from an
// address that is not a configured peer, or while BGP does not run, is closed without sending anything; one arriving
// while a connection is already pending for that peer as well.
//
// RIPng. Updates go to ff02::9 port 521 (the task sends from the backbone link-local address with hop limit 255).
// Requests are answered unicast to the requester's address and port, whatever the port. Responses and invalid
// datagrams are counted in responses_dropped and dropped. While the border router is down the scheduler is stopped
// (every prefix sent once at metric 16) and restarted with a full update when it is up again.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "announce.h"
#include "bgp_fsm.h"
#include "bgp_msg.h"
#include "ripng.h"
#include "routing_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ROUTING_BGP_PORT 179
#define ROUTING_BGP_CEASE_CONFIG_CHANGE 6 /**< RFC 4486 Other Configuration Change */

/** routing_status_t.bgp_blocked texts. */
#define ROUTING_BLOCKED_NO_ROUTER_ID "no router id"
#define ROUTING_BLOCKED_NO_LINK_LOCAL "no link-local address"

/** What the task does for the engine. Every op is required. */
typedef struct {
    void *ctx;
    /**
     * Start a non-blocking TCP connection to `addr` port 179 on the backbone (link-local addresses in its zone).
     * Returns a handle >= 0 (the task reports the outcome with routing_engine_tcp_connected), or -1 with `*why` set to
     * a short static text when it failed at once.
     */
    int (*tcp_connect)(void *ctx, const uint8_t addr[16], const char **why);
    /** Queue `len` bytes on the connection. 0 on success, -1 when the connection is broken. */
    int (*tcp_send)(void *ctx, int h, const uint8_t *buf, size_t len);
    /** Close the connection (queued data still goes out). The handle is not reported again. */
    void (*tcp_close)(void *ctx, int h);
    /** Open (`on`) or close the BGP listening socket. Opening returns false on failure (retried at the next poll). */
    bool (*bgp_listen)(void *ctx, bool on);
    /** Open (`on`) or close the RIPng socket. Opening returns false on failure (retried at the next poll). */
    bool (*ripng_socket)(void *ctx, bool on);
    /** Send one RIPng datagram: to ff02::9 port 521 when `dst` is NULL, else to `dst` port `port`. 0 on success. */
    int (*ripng_send)(void *ctx, const uint8_t *dst, uint16_t port, const uint8_t *buf, size_t len);
    /** routing_publisher_apply under the OpenThread lock. false: not done (retried at the next poll). */
    bool (*publisher_apply)(void *ctx, const routing_settings_t *s);
    /** Random numbers for the RIPng update offset. */
    uint32_t (*rand32)(void);
} routing_engine_io_t;

/** The OpenThread and backbone state the task reads every 500 ms. */
typedef struct {
    bool br_up; /**< border router running (BR_RUNNING) and Thread attached (child, router or leader) */
    bool ula_known;
    routing_prefix64_t ula; /**< the self-generated ULA */
    bool custom_in_netdata; /**< the custom OMR prefix of the settings is in Network Data */
    bool ula_in_netdata;
    bool pd_known;
    routing_prefix64_t pd_prefix; /**< the delegated OMR prefix (otBorderRoutingGetPdOmrPrefix) */
    uint32_t backbone_ipv4;       /**< host order, 0 when the backbone has none */
    bool have_link_local;
    routing_bgp_nexthop_t nexthop; /**< the backbone global (if any) and link-local addresses */
} routing_engine_state_t;

/** One TCP connection of a peer; h -1 when unused. Private. */
typedef struct {
    int h;
    bool connecting; /**< an outgoing attempt not yet reported connected */
    uint16_t len;
    uint8_t buf[ROUTING_BGP_MAX_MSG];
} routing_engine_conn_t;

/** One configured peer. Private. */
typedef struct {
    bool in_use;
    routing_bgp_peer_t cfg;
    routing_bgp_fsm_t fsm;
    routing_engine_conn_t conn[2]; /**< conn[cur] is the current connection, conn[cur ^ 1] the pending incoming one */
    uint8_t cur;
    routing_prefix_set_t sent; /**< what this session announced last */
} routing_engine_peer_t;

/** Engine state; the caller owns the storage (about 22 KB). Fields are private to engine.c. */
typedef struct {
    const routing_engine_io_t *io;
    routing_settings_t settings;
    bool started;
    bool publisher_dirty;
    bool have_state;
    routing_engine_state_t state;
    routing_prefix64_t state_custom; /**< the custom prefix state.custom_in_netdata was read for */
    routing_prefix_set_t announced;
    /* BGP */
    bool bgp_running;
    bool listening;
    uint32_t router_id; /**< in use by the sessions, 0 none */
    const char *blocked;
    routing_engine_peer_t peers[ROUTING_MAX_PEERS];
    uint8_t tx[ROUTING_BGP_MAX_MSG];
    /* RIPng */
    bool ripng_running;
    bool ripng_open;
    bool sched_on;
    routing_ripng_sched_t sched;
    bool ripng_ever;
    uint32_t ripng_last_ms;
    routing_ripng_status_t ripng;
    uint8_t ripng_tx[ROUTING_RIPNG_MAX_PAYLOAD];
} routing_engine_t;

/** Idle engine with settings `s`; nothing runs and no op is called until routing_engine_start. */
void routing_engine_init(routing_engine_t *e, const routing_engine_io_t *io, const routing_settings_t *s);

/**
 * Start the sender of the method: RIPng opens its socket (its scheduler starts once the border router is up); BGP
 * waits for the first poll. The publisher is not applied: routing_on_border_router_up applies the stored settings.
 */
void routing_engine_start(routing_engine_t *e, uint32_t now_ms);

/** New settings (already validated and stored), handled as the header comment says. No-op when nothing differs. */
void routing_engine_set_settings(routing_engine_t *e, const routing_settings_t *s, uint32_t now_ms);

/**
 * The state read every 500 ms: recomputes the announcement set (on a change: BGP sessions send the difference, RIPng a
 * triggered update; the border router going down stops RIPng with metric 16), retries a failed publisher apply or
 * socket open, starts, stops or restarts BGP as the router ID allows, then runs routing_engine_tick.
 */
void routing_engine_poll(routing_engine_t *e, const routing_engine_state_t *st, uint32_t now_ms);

/** Runs the BGP timers and the RIPng scheduler that are due. Call it after every wake-up. */
void routing_engine_tick(routing_engine_t *e, uint32_t now_ms);

/** The earliest BGP or RIPng deadline, UINT32_MAX when none runs. */
uint32_t routing_engine_next_deadline_ms(const routing_engine_t *e);

/** An incoming connection was accepted on the backbone from `addr` (the task closed those from elsewhere). */
void routing_engine_tcp_accepted(routing_engine_t *e, int h, const uint8_t addr[16], uint32_t now_ms);

/** The outgoing attempt `h` connected (`ok`) or failed (`why`, a short static text, may be NULL). */
void routing_engine_tcp_connected(routing_engine_t *e, int h, bool ok, const char *why, uint32_t now_ms);

/**
 * Where the task reads the next bytes of `h` into: sets `*buf` and returns the room left, 0 (and `*buf` NULL) for an
 * unknown or connecting handle.
 */
size_t routing_engine_tcp_rx_space(routing_engine_t *e, int h, uint8_t **buf);

/** `n` > 0: that many bytes were written at the rx_space buffer; 0: the peer closed; < 0: the connection failed. */
void routing_engine_tcp_rx(routing_engine_t *e, int h, int n, uint32_t now_ms);

/** A RIPng datagram from `src` port `port` arrived on the backbone. Ignored unless RIPng runs. */
void routing_engine_ripng_rx(routing_engine_t *e, const uint8_t src[16], uint16_t port, const uint8_t *buf,
                             size_t len, uint32_t now_ms);

/** The settings the engine runs. */
const routing_settings_t *routing_engine_settings(const routing_engine_t *e);

/**
 * The status: OMR prefix in use (custom; else the delegated prefix; else the ULA), ULA, announcement set, router ID in
 * use and blocked reason (method bgp only), peers[i] for settings peer i, RIPng counters.
 */
void routing_engine_status(const routing_engine_t *e, uint32_t now_ms, routing_status_t *out);

void routing_engine_metrics_view(const routing_engine_t *e, uint32_t now_ms, routing_metrics_view_t *out);

#ifdef __cplusplus
}
#endif
