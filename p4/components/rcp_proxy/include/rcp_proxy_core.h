// Decisions of the RCP proxy (the rcp-proxy mode of a radio channel). Pure: no ESP-IDF calls.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "radio_mux_core.h"

#ifdef __cplusplus
extern "C" {
#endif

// Smallest radio -> peer ring of a TCP peer: above the 5760-byte TCP send buffer and one encoded 2048-byte frame.
#define RCP_PROXY_RING_MIN 8192
// TCP connect back-off: 1 s doubling to this ceiling; back to 1 s after a connection that lasted RESET_MS.
#define RCP_PROXY_BACKOFF_MAX_S 30
#define RCP_PROXY_BACKOFF_RESET_MS 60000
// UDP listen: the locked source is let go after this much silence.
#define RCP_PROXY_UDP_SILENCE_MS 30000
// UDP framing raw: the largest datagram towards the peer.
#define RCP_PROXY_UDP_RAW_MAX 1024

// True for "<name>.local" (any case) with a non-empty name: copies the name without ".local" into out.
bool rcp_proxy_host_mdns(const char *host, char *out, size_t cap);

// The next connect back-off in seconds: 1, 2, 4, 8, 16, 30, 30 ... for consecutive failures (prev_s 0 = none yet);
// 1 again when the connection that just ended lasted RCP_PROXY_BACKOFF_RESET_MS or more (connected_for_ms; 0 or
// negative when it never connected).
uint32_t rcp_proxy_backoff_next(uint32_t prev_s, int64_t connected_for_ms);

// A peer address: IPv6, IPv4 as ::ffff:a.b.c.d; scope is the zone of a link-local address (else 0).
typedef struct {
    uint8_t ip[16];
    uint16_t port;
    uint32_t scope;
} rcp_proxy_addr_t;

bool rcp_proxy_addr_eq(const rcp_proxy_addr_t *a, const rcp_proxy_addr_t *b);

// UDP listen: the first source becomes the peer until it has been silent for RCP_PROXY_UDP_SILENCE_MS or is kicked.
// (Connect locks to the configured target the same way.) A peer whose datagram a radio write cut part-way
// (RCP_PROXY_TX_END: raw bytes on a dongle) is let go and barred for RCP_PROXY_UDP_BAR_MS: its next datagrams would
// continue the cut stream, spliced onto the partial write.
#define RCP_PROXY_UDP_BAR_MS 2000

typedef struct {
    bool locked;
    rcp_proxy_addr_t peer;
    int64_t last_ms;  // when the peer last sent
    uint32_t refused; // datagrams refused from other sources (and from the barred one)
    bool barred;      // `bar` is refused from bar_ms for RCP_PROXY_UDP_BAR_MS
    rcp_proxy_addr_t bar;
    int64_t bar_ms;
} rcp_proxy_udp_lock_t;

void rcp_proxy_udp_lock_init(rcp_proxy_udp_lock_t *lock);
// A datagram from src at now_ms: true when it is taken (it locked an unlocked or expired lock, or came from the
// peer); false and refused + 1 when another source holds the lock, or src is barred (less than RCP_PROXY_UDP_BAR_MS
// after its bar; a clock that went back ends the bar).
bool rcp_proxy_udp_lock_offer(rcp_proxy_udp_lock_t *lock, const rcp_proxy_addr_t *src, int64_t now_ms);
void rcp_proxy_udp_lock_kick(rcp_proxy_udp_lock_t *lock);
// A radio write cut the peer's datagram part-way at now_ms: unlocked, and the peer barred (rcp_proxy_udp_lock_offer).
// Nothing when unlocked.
void rcp_proxy_udp_lock_bar(rcp_proxy_udp_lock_t *lock, int64_t now_ms);
// Unlocks after the silence; true when this call unlocked it.
bool rcp_proxy_udp_lock_expire(rcp_proxy_udp_lock_t *lock, int64_t now_ms);
// When the lock expires; -1 when unlocked.
int64_t rcp_proxy_udp_lock_deadline(const rcp_proxy_udp_lock_t *lock);

// UDP listen: sources the netcfg access rules refuse. The last refused source and its reason are kept for
// RCP_PROXY_REFUSAL_CACHE_MS, so a stream from it is refused without another rule lookup (the netcfg lock and a route
// lookup on the tcpip task per datagram); the refusals are logged at most once per RCP_PROXY_REFUSAL_LOG_MS, with
// their count.
#define RCP_PROXY_REFUSAL_CACHE_MS 1000
#define RCP_PROXY_REFUSAL_LOG_MS 10000

typedef struct {
    bool cached;
    rcp_proxy_addr_t addr; // the last refused source
    const char *why;       // its reason (a string literal)
    int64_t at_ms;         // when the rules refused it
    bool logged;           // a log line was written (log_ms is valid)
    int64_t log_ms;        // the last log line
    uint32_t count;        // refusals since the last log line
} rcp_proxy_refusal_t;

void rcp_proxy_refusal_init(rcp_proxy_refusal_t *r);
// Whether src was refused by the rules within the last RCP_PROXY_REFUSAL_CACHE_MS (a clock that went back: no).
bool rcp_proxy_refusal_cached(const rcp_proxy_refusal_t *r, const rcp_proxy_addr_t *src, int64_t now_ms);
// The rules refused src at now_ms for `why`: kept as the last refused source.
void rcp_proxy_refusal_note(rcp_proxy_refusal_t *r, const rcp_proxy_addr_t *src, const char *why, int64_t now_ms);
// Counts one refused datagram at now_ms. Returns how many to report in a log line now (this one included), or 0 while
// the last line is younger than RCP_PROXY_REFUSAL_LOG_MS (a clock that went back logs at once).
uint32_t rcp_proxy_refusal_log(rcp_proxy_refusal_t *r, int64_t now_ms);

// Whether a peer that just became the session's peer starts a new peer session, which replays the radio's reset
// handshake (frame channels). TCP: every accept and every connect is new (init before each). UDP: init once per socket
// session; connect is then new once, and listen only when the newly locked source differs from the previous peer, so
// a host that was silent for 30 s and sends again keeps its session.
typedef struct {
    bool had;              // a peer was seen in this socket session
    rcp_proxy_addr_t last; // that peer
} rcp_proxy_peer_memo_t;

void rcp_proxy_peer_memo_init(rcp_proxy_peer_memo_t *m);
// Records peer; true when it starts a new peer session (no peer before, or a different one).
bool rcp_proxy_peer_new(rcp_proxy_peer_memo_t *m, const rcp_proxy_addr_t *peer);

// A new peer session's replayed reset (frame channels): the peer's frames are dropped until the radio reports the reset
// done (RADIO_MUX_EV_RESET), or RCP_PROXY_REPLAY_MAX_MS passed. Passed at once, a spinel host's RESET could reach the
// RCP before the reset, be answered there, and the host's next request be lost in the reset (otbr-agent then gives
// up); dropped, the host's RESET is answered by the reset's own notification.
#define RCP_PROXY_REPLAY_MAX_MS 1000

typedef struct {
    bool active;
    int64_t start_ms; // when the replayed reset was posted
} rcp_proxy_replay_t;

// None pending (a new session, or one that replays nothing).
void rcp_proxy_replay_init(rcp_proxy_replay_t *r);
// The replayed reset was posted at now_ms.
void rcp_proxy_replay_start(rcp_proxy_replay_t *r, int64_t now_ms);
// The radio reported the reset done.
void rcp_proxy_replay_done(rcp_proxy_replay_t *r);
// Whether a frame from the peer at now_ms is dropped: a replay is pending and less than RCP_PROXY_REPLAY_MAX_MS old.
// Past that (or with a clock that went back) the replay is over.
bool rcp_proxy_replay_blocks(rcp_proxy_replay_t *r, int64_t now_ms);

// Connect: the target's name lookup runs off the proxy task (rcp_resolve) and its answer comes back later. A session
// waits for the answer to its latest ask only: an answer to an earlier ask, or one that comes after the session
// stopped waiting (a retry, a close, new settings: rcp_proxy_lookup_cancel, or a new session), is discarded.
typedef struct {
    uint32_t pending; // the ask the session waits for; 0 none
} rcp_proxy_lookup_t;

// A new ask: its number (never 0) from the counter *seq, which every session of the task shares so a number is never
// reused by a later session; the session now waits for it (an earlier ask is forgotten).
uint32_t rcp_proxy_lookup_ask(rcp_proxy_lookup_t *l, uint32_t *seq);
// The session stops waiting: any answer that comes is discarded.
void rcp_proxy_lookup_cancel(rcp_proxy_lookup_t *l);
// Whether the session waits for an answer.
bool rcp_proxy_lookup_waiting(const rcp_proxy_lookup_t *l);
// An answer to ask `seq` came: true (and the session no longer waits) when it is the one the session waits for; false
// when it is to be discarded.
bool rcp_proxy_lookup_answer(rcp_proxy_lookup_t *l, uint32_t seq);

typedef enum {
    RCP_PROXY_TAKE,    // the slot is free: hold the new client
    RCP_PROXY_REFUSE,  // close the new client
    RCP_PROXY_REPLACE, // close the held client that has sent nothing (an idle preconnect), hold the new one
} rcp_proxy_accept_t;

// TCP listen: one client at a time (slot_taken: the peer, or a held client). The first client keeps the port once it
// sent bytes; a held client that has sent nothing yet (held_silent) gives way to a new connection, so an idle
// connection (a browser's preconnect) never locks out the real host. A connection must also pass both netcfg access
// rules: by the local address it came in on (local_allowed, netcfg_socket_allowed) and by the route back to the
// client (peer_allowed, netcfg_peer_allowed: a client on the Thread mesh is refused even when it connected to an
// infrastructure address); a refused one never replaces the held client.
rcp_proxy_accept_t rcp_proxy_accept(bool slot_taken, bool held_silent, bool local_allowed, bool peer_allowed);

// TCP listen: the first bytes a new client ever sends are held until RCP_PROXY_SNIFF_LEN of them came or
// RCP_PROXY_SNIFF_MS passed since the first of them, whichever is first (earlier when they already decide it), so a
// web page's cross-protocol request (a POST to http://<board>:6638/ from any LAN browser, also over a connection the
// browser opened earlier and kept idle: a preconnect) never reaches the radio. A client that has sent nothing is never
// the peer: no radio bytes, no reset replay, no busy flag; it stays held (rcp_proxy_accept: a new connection replaces
// it), and becomes the peer when its first bytes pass.
#define RCP_PROXY_SNIFF_LEN 8
#define RCP_PROXY_SNIFF_MS 50

typedef enum {
    RCP_PROXY_SNIFF_WAIT,   // undecided: hold the bytes, wait for more or the timeout
    RCP_PROXY_SNIFF_PASS,   // a radio client: the held bytes and the rest go to the radio
    RCP_PROXY_SNIFF_REFUSE, // an HTTP request line: drop the client, nothing reaches the radio
} rcp_proxy_sniff_t;

// The held first n bytes of a client (n <= RCP_PROXY_SNIFF_LEN; timed_out once RCP_PROXY_SNIFF_MS passed since the
// first of them): REFUSE when they start with an uppercase HTTP method (GET, POST, PUT, DELETE, HEAD, OPTIONS, PATCH,
// CONNECT, TRACE) and a space; PASS as soon as they can no longer become one (spinel starts with 0x7E, ZNP with 0xFE,
// ASH with 0x1A or 0xC0 ...), or at the timeout or at RCP_PROXY_SNIFF_LEN bytes without a match ("GET" alone); WAIT
// while they are still a method's prefix, and always while there are none (n 0), timed out or not.
rcp_proxy_sniff_t rcp_proxy_sniff(const uint8_t *d, size_t n, bool timed_out);

// The hold of a TCP listen client's first bytes, from the accept until they are judged (rcp_proxy_sniff).
typedef struct {
    uint8_t buf[RCP_PROXY_SNIFF_LEN];
    size_t len;          // bytes held
    int64_t deadline_ms; // the judgement's timer, from the first byte; -1 none (nothing came yet, or judged)
} rcp_proxy_hold_t;

typedef enum {
    RCP_PROXY_HOLD_WAIT,   // nothing to do yet
    RCP_PROXY_HOLD_PASS,   // judged a radio client: the peer now, then buf[0..len) to the radio
    RCP_PROXY_HOLD_REFUSE, // judged an HTTP request: drop the client; nothing to the radio
} rcp_proxy_hold_act_t;

// At the accept: nothing held, no timer until the first byte.
void rcp_proxy_hold_start(rcp_proxy_hold_t *h);
// Whether the held client has sent nothing yet (a new connection replaces it: rcp_proxy_accept).
bool rcp_proxy_hold_silent(const rcp_proxy_hold_t *h);
// Room for more bytes: read at most this much from the client (0 never happens while undecided).
size_t rcp_proxy_hold_room(const rcp_proxy_hold_t *h);
// n bytes (n <= rcp_proxy_hold_room) came at now_ms: the first of them starts the judgement's RCP_PROXY_SNIFF_MS.
// WAIT, PASS or REFUSE.
rcp_proxy_hold_act_t rcp_proxy_hold_add(rcp_proxy_hold_t *h, const uint8_t *d, size_t n, int64_t now_ms);
// The timer at now_ms: WAIT without a deadline (nothing came: the client may stay silent for ever) or before it; at it,
// the judgement with timed_out (PASS or REFUSE).
rcp_proxy_hold_act_t rcp_proxy_hold_tick(rcp_proxy_hold_t *h, int64_t now_ms);

typedef enum {
    RCP_PROXY_TX_DISCARD, // nothing was written: count the bytes as discarded, the peer session goes on
    RCP_PROXY_TX_END,     // the radio may have taken part of the bytes: end the peer session
} rcp_proxy_tx_fail_t;

// A byte channel's (raw) radio_mux_tx failed with err: ESP_ERR_INVALID_STATE means the channel is not attached right
// now and nothing was written (DISCARD; radio_mux_tx returns it only from its own checks, a transport's becomes
// ESP_FAIL); any other error (a USB write timeout halts the endpoint part-way through) may have written a part, and
// going on would splice the peer's byte stream (a ZNP or EZSP dongle would see a frame cut into the next one), so the
// session ends (END). Frame channels never ask: HDLC resyncs at the next flag.
rcp_proxy_tx_fail_t rcp_proxy_raw_tx_failed(esp_err_t err);

// Byte FIFO over caller-owned storage. Not thread-safe: one task owns it.
typedef struct {
    uint8_t *data;
    size_t cap, head, len;
} rcp_proxy_ring_t;

void rcp_proxy_ring_init(rcp_proxy_ring_t *r, uint8_t *storage, size_t cap);
// All or nothing: false (ring unchanged) when n bytes do not fit.
bool rcp_proxy_ring_push(rcp_proxy_ring_t *r, const uint8_t *src, size_t n);
// Takes up to cap bytes in push order; returns how many.
size_t rcp_proxy_ring_take(rcp_proxy_ring_t *r, uint8_t *out, size_t cap);
// The contiguous run at the head; rcp_proxy_ring_drop removes n bytes of it (after they were sent).
size_t rcp_proxy_ring_peek(const rcp_proxy_ring_t *r, const uint8_t **p);
void rcp_proxy_ring_drop(rcp_proxy_ring_t *r, size_t n);
void rcp_proxy_ring_clear(rcp_proxy_ring_t *r);

// UDP framing hdlc on a byte channel: cuts the radio's byte stream into HDLC frames (radio_mux_hdlc_chunk), one
// datagram each, flags included. The buffer is bounded: when it is full without a complete frame its bytes are
// dropped (a trailing 0x7E is kept, it may open the next frame) and overflows + 1.
typedef struct {
    uint8_t *buf;
    size_t cap, len;
    uint32_t overflows;
} rcp_proxy_cut_t;

typedef void (*rcp_proxy_emit_fn)(void *ctx, const uint8_t *frame, size_t len);

void rcp_proxy_cut_init(rcp_proxy_cut_t *c, uint8_t *buf, size_t cap);
// Feeds n bytes; emit is called once per complete frame in stream order. Returns the frames emitted.
size_t rcp_proxy_cut_feed(rcp_proxy_cut_t *c, const uint8_t *in, size_t n, rcp_proxy_emit_fn emit, void *ctx);
void rcp_proxy_cut_clear(rcp_proxy_cut_t *c);

/* ---- the orchestrator instance (one per channel in rcp-proxy) ---- */

typedef enum {
    RCP_PROXY_REINIT_ACCEPT,      // no peer: the session and the listener close, the channel is let go
    RCP_PROXY_REINIT_REFUSE_PEER, // a peer is connected: maintenance is refused (409 c6_busy)
} rcp_proxy_reinit_t;

// The proxy's answer to a reinit request (maintenance wants its channel).
rcp_proxy_reinit_t rcp_proxy_reinit_verdict(bool peer_connected);

// Whether an input with seq `input_seq` is newer than the last one handled (`seen_seq`), the seq wrapping at 2^32:
// false for the same input again or an older one.
bool rcp_proxy_input_current(uint32_t seen_seq, uint32_t input_seq);

// After the proxy answered a REINIT that arrived on top of a bind (BIND or BACK, `bind_seq`) it had not acted on yet
// (`bind_seen` is the last bind it acted on): whether it acts on that bind now. Only after a refusal: a REINIT it let go
// for supersedes the earlier bind (BACK binds it again at the maintenance's end).
bool rcp_proxy_bind_after_reinit(bool accepted, uint32_t bind_seq, uint32_t bind_seen);

#ifdef __cplusplus
}
#endif
