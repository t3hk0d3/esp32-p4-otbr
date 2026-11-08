// Shared between the proxy task (proxy.c) and the network adapters (adapter_tcp.c, adapter_udp.c). Everything here runs
// on the proxy task only.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/select.h>

#include "esp_err.h"
#include "lwip/sockets.h"
#include "radio_mux.h"
#include "radio_mux_hdlc.h"
#include "rcp_proxy.h"
#include "rcp_proxy_core.h"

typedef enum {
    PROXY_IDLE,       // no channel
    PROXY_LISTENING,  // listen: waiting for a client or a first source
    PROXY_CONNECTING, // connect: dialling (TCP), or waiting for the peer's first datagram (UDP)
    PROXY_BACKOFF,    // the adapter failed to open or lost its connection: opened again at retry_ms
    PROXY_CONNECTED,  // a peer
} proxy_state_t;

typedef struct proxy proxy_t;

// A network adapter: one way to reach the peer. A stream adapter carries bytes; a datagram adapter carries one HDLC
// frame (framing hdlc) or up to RCP_PROXY_UDP_RAW_MAX bytes (framing raw) per datagram.
typedef struct {
    const char *name;
    bool datagram;
    // Opens the sockets for p->set (listen, or resolve and dial). An error: proxy.c closes and backs off. A connect
    // target that is a name is resolved off the task (rcp_proxy_resolve): open then returns ESP_OK in CONNECTING
    // without a socket, and proxy.c calls dial once the answer came and passed the access rules.
    esp_err_t (*open)(proxy_t *p);
    // Connect: p->dst is resolved and allowed; opens the socket towards it (TCP: a non-blocking dial). An error:
    // proxy.c closes and backs off.
    esp_err_t (*dial)(proxy_t *p);
    // Closes every socket; also after a failed open.
    void (*close)(proxy_t *p);
    // Adds the sockets to wait on (wr: a connect in progress, or bytes waiting for room) and sets p->wake_ms to the
    // adapter's next timer (-1 none). Returns the new max fd.
    int (*fds)(proxy_t *p, fd_set *rd, fd_set *wr, int maxfd);
    // After every wait (also on a timeout): handles the ready sockets and the timers.
    void (*ready)(proxy_t *p, fd_set *rd, fd_set *wr);
    // Towards the peer: bytes (stream) or one datagram. ESP_ERR_INVALID_STATE when there is no peer to send to.
    esp_err_t (*send)(proxy_t *p, const uint8_t *d, size_t n);
    // Drops the current peer: a kick, or (`cut`) a radio write that cut the peer's bytes part-way (RCP_PROXY_TX_END),
    // after which its stream must not go on (TCP: disconnected anyway; UDP: its source barred RCP_PROXY_UDP_BAR_MS).
    void (*drop)(proxy_t *p, bool cut);
} rcp_proxy_adapter_t;

extern const rcp_proxy_adapter_t rcp_proxy_adapter_tcp;
extern const rcp_proxy_adapter_t rcp_proxy_adapter_udp;

struct proxy {
    radio_mux_chan_t *ch; // NULL: idle
    char id[RADIO_MUX_ID_MAX];
    bool frames;          // a frame channel (radio_mux_chan_frames: the C6): spinel frames in, HDLC towards the peer
    radio_mux_proxy_settings_t set;
    const rcp_proxy_adapter_t *ad;
    proxy_state_t state;
    char peer[RADIO_MUX_PEER_MAX];
    char last_error[RADIO_MUX_PROXY_ERROR_MAX]; // the last failed open or connect (rcp_proxy_status_t), "" none
    int64_t wake_ms;  // the adapter's next timer, -1 none
    uint32_t backoff_s;
    int64_t retry_ms; // BACKOFF: when to open again
    int64_t up_ms;    // when the peer connected (the back-off resets after a long connection)

    // Adapter state.
    int lfd; // TCP listener
    int fd;  // TCP client or connection; the UDP socket
    struct sockaddr_storage dst; // connect: the resolved target; UDP listen: the locked source
    socklen_t dst_len;
    int64_t conn_deadline_ms; // TCP connect in progress: give up at
    // TCP listen: a client accepted whose first bytes are not judged yet (rcp_proxy_hold_t). Not the peer until they
    // pass; while it has sent nothing, a new connection replaces it (rcp_proxy_accept).
    bool sniffing;
    rcp_proxy_hold_t hold;
    struct sockaddr_storage sniff_from;
    int64_t http_log_ms; // the last log of a refused HTTP request (rate-limited)
    rcp_proxy_lookup_t lookup; // connect: the name lookup the session waits for (the resolver task answers)
    rcp_proxy_udp_lock_t lock;
    rcp_proxy_refusal_t refusal; // UDP listen: the last source the access rules refused, and the refusals' log
    rcp_proxy_peer_memo_t memo; // the previous peer of this socket session (UDP: reset replay only for a new one)

    // Buffers of the session (allocated when it opens).
    uint8_t *ring_buf; // TCP: radio -> peer bytes the socket has not taken yet
    rcp_proxy_ring_t ring;
    uint8_t *enc_buf; // frame channel: one encoded frame
    uint8_t *dec_buf; // frame channel: the peer's frame being decoded
    radio_mux_hdlc_dec_t dec;
    uint8_t *cut_buf; // byte channel over UDP framing hdlc: the stream being cut into frames
    rcp_proxy_cut_t cut;

    // Counters since the session opened (logged).
    uint64_t to_radio, to_peer;
    uint32_t discarded, refused, overflows, tx_errors;
};

// The task's receive buffer (one task, so one buffer for every adapter): datagrams up to one encoded 2048-byte frame.
#define RCP_PROXY_RX_MAX (RADIO_MUX_HDLC_ENCODED_MAX(RADIO_MUX_FRAME_MAX) + 64)
uint8_t *rcp_proxy_rx_buf(void);

int64_t rcp_proxy_now_ms(void);
// A peer is there: state CONNECTED, its address as text, the session's buffers cleared. When it starts a new peer
// session (new_session, rcp_proxy_peer_new) on a frame channel, the radio is reset, so the peer's host starts from the
// reset handshake.
void rcp_proxy_peer_up(proxy_t *p, const struct sockaddr *peer, bool new_session);
// The peer left: back to `st` (LISTENING or CONNECTING) without a peer.
void rcp_proxy_peer_lost(proxy_t *p, proxy_state_t st);
// Closes the adapter and opens it again after the back-off (connect failed, the connection was lost, a listener
// could not be opened). A failure to open or connect (not a lost connection) becomes the session's last_error, unless
// the adapter noted a more precise one (rcp_proxy_note_error) for this try.
void rcp_proxy_retry(proxy_t *p, const char *why);
// The reason this try to open or connect failed, for the status (printf format; cut to RADIO_MUX_PROXY_ERROR_MAX).
void rcp_proxy_note_error(proxy_t *p, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
// The peer's bytes (a stream chunk or one datagram) towards the radio.
void rcp_proxy_from_peer(proxy_t *p, const uint8_t *d, size_t n);
// Resolves p->set.host and p->set.port into p->dst without blocking the proxy task. An IP literal at once: ESP_OK, or
// ESP_ERR_NOT_ALLOWED when the netcfg access rules refuse the target (netcfg_peer_allowed, the same rule as for UDP
// sources). A name (DNS, getaddrinfo; .local over mDNS, A then AAAA) is asked of the resolver task (rcp_resolve):
// ESP_ERR_NOT_FINISHED, the session stays CONNECTING, and when the answer comes proxy.c applies the access rules and
// calls the adapter's dial, or backs off with the reason. An answer for a session that stopped waiting meanwhile (a
// retry, a close, new settings) is discarded.
esp_err_t rcp_proxy_resolve(proxy_t *p, int socktype);
void rcp_proxy_addr_from(const struct sockaddr *sa, rcp_proxy_addr_t *out);
// "a.b.c.d:port" (IPv4, also mapped on a dual-stack socket) or "[addr]:port".
void rcp_proxy_peer_text(const struct sockaddr *sa, char *out, size_t cap);
void rcp_proxy_set_nonblocking(int fd);
