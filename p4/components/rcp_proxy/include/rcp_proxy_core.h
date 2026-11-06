// Decisions of the RCP proxy (the rcp-proxy mode of a radio channel). Pure: no ESP-IDF calls.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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

typedef enum {
    RCP_PROXY_OK,
    RCP_PROXY_NEED_HOST, // connect without a host
    RCP_PROXY_BAD_PORT,  // 0, 80 or 179 (the board's own listeners)
    RCP_PROXY_BAD_HOST,  // not an IPv4 or IPv6 literal, nor a DNS or .local name of at most 63 characters
} rcp_proxy_check_t;

// The full check of the rcp_proxy settings (device_api runs it before radio_mux_set): port, then a host for connect,
// then the host's syntax when one is given (in either direction: a host kept for a later switch to connect is valid).
// The framing is not checked against the adapter: it is kept with tcp for a later switch to udp.
rcp_proxy_check_t rcp_proxy_settings_check(const radio_mux_proxy_settings_t *s);

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
typedef struct {
    bool locked;
    rcp_proxy_addr_t peer;
    int64_t last_ms;  // when the peer last sent
    uint32_t refused; // datagrams refused from other sources
} rcp_proxy_udp_lock_t;

void rcp_proxy_udp_lock_init(rcp_proxy_udp_lock_t *lock);
// A datagram from src at now_ms: true when it is taken (it locked an unlocked or expired lock, or came from the
// peer); false and refused + 1 when another source holds the lock.
bool rcp_proxy_udp_lock_offer(rcp_proxy_udp_lock_t *lock, const rcp_proxy_addr_t *src, int64_t now_ms);
void rcp_proxy_udp_lock_kick(rcp_proxy_udp_lock_t *lock);
// Unlocks after the silence; true when this call unlocked it.
bool rcp_proxy_udp_lock_expire(rcp_proxy_udp_lock_t *lock, int64_t now_ms);
// When the lock expires; -1 when unlocked.
int64_t rcp_proxy_udp_lock_deadline(const rcp_proxy_udp_lock_t *lock);

typedef enum {
    RCP_PROXY_TAKE,
    RCP_PROXY_REFUSE,
} rcp_proxy_accept_t;

// TCP listen: the first client keeps the port; a connection while one is connected is refused.
rcp_proxy_accept_t rcp_proxy_accept(bool peer_connected);

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

#ifdef __cplusplus
}
#endif
