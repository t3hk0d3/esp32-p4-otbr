// TCP adapter (from usb_serial's tcp.c): listen (first client wins, later ones are accepted and closed at once) or
// connect (non-blocking dial to a target the netcfg rules allow, back-off on failure). A listen client's first bytes,
// whenever they come, are held until rcp_proxy_sniff decides (rcp_proxy_hold_t: at most RCP_PROXY_SNIFF_LEN bytes or
// RCP_PROXY_SNIFF_MS from the first byte): an HTTP request line is refused before anything reaches the radio. A client
// silent RCP_PROXY_SNIFF_MS after the accept becomes the peer meanwhile (the radio's bytes reach it), its own bytes
// still held. Clients get TCP_NODELAY and keepalive 10/2/3; every accepted (passed or silent) client and every connect
// is a new peer session (reset replay on a frame channel). Bytes towards the peer wait in the session's ring while the
// socket has no room; a ring that would overflow disconnects the peer.
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "esp_log.h"
#include "lwip/sockets.h"
#include "netcfg.h"
#include "rcp_proxy_priv.h"

static const char *TAG = "rcp_proxy";

#define LISTEN_BACKLOG 1
#define KEEP_IDLE_S 10
#define KEEP_INTVL_S 2
#define KEEP_COUNT 3
#define CONNECT_TIMEOUT_MS 10000
#define RECV_CHUNK 1024
#define HTTP_LOG_MS 10000 // a refused HTTP request is logged at most this often

static bool connecting(const proxy_t *p)
{
    return p->state == PROXY_CONNECTING && p->fd >= 0;
}

// A listener on `port`; -1 with the reason in p's last_error.
static int listen_on(proxy_t *p, uint16_t port)
{
    const int fd = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
    const int one = 1;
    const int zero = 0;

    if (fd < 0) {
        ESP_LOGE(TAG, "socket: errno %d", errno);
        rcp_proxy_note_error(p, "tcp socket: %s", strerror(errno));
        return -1;
    }
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof(zero)); // dual-stack: IPv4 clients too
    struct sockaddr_in6 sa = {
        .sin6_family = AF_INET6,
        .sin6_port = htons(port),
        .sin6_addr = IN6ADDR_ANY_INIT,
    };

    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0 || listen(fd, LISTEN_BACKLOG) != 0) {
        const int err = errno;

        ESP_LOGE(TAG, "listening on TCP port %u: errno %d", port, err);
        rcp_proxy_note_error(p, "tcp port %u: %s", port, strerror(err));
        close(fd);
        return -1;
    }
    rcp_proxy_set_nonblocking(fd);
    return fd;
}

// Closes a refused client. lwIP has no reset for it from the socket API: SO_LINGER {1, 0} aborts only a connection
// with unsent or unacknowledged data (api_msg.c), and a refused client was never written to. close() sends an RST when
// bytes of the client are still unread (tcp_close_shutdown), else a FIN. The socket is free at once; the PCB goes
// through FIN_WAIT and TIME_WAIT, and lwIP takes such PCBs back (TIME_WAIT, then FIN_WAIT_2 and FIN_WAIT_1, oldest
// first) when it needs one for a new connection (tcp_alloc), so refused clients do not use up CONFIG_LWIP_MAX_ACTIVE_TCP.
static void refuse(int fd)
{
    close(fd);
}

static void peer_setup(int fd)
{
    const int one = 1;
    const int idle = KEEP_IDLE_S;
    const int intvl = KEEP_INTVL_S;
    const int count = KEEP_COUNT;

    // Nagle plus delayed ACK would add ~200 ms stalls to spinel and ASH exchanges.
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    // A vanished peer is closed within about 16 s instead of lwIP's two hours.
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count));
    rcp_proxy_set_nonblocking(fd);
}

// The peer is gone (closed, failed, kicked, too slow): listen waits for the next client, connect dials again after
// the back-off.
static void lose_peer(proxy_t *p, const char *why)
{
    if (p->sniffing && p->state != PROXY_CONNECTED) { // not a peer yet: the radio and the status never saw it
        p->sniffing = false;
        close(p->fd);
        p->fd = -1;
        ESP_LOGD(TAG, "%s: a new client: %s", p->id, why);
        return;
    }
    p->sniffing = false;
    if (p->set.dir == RADIO_MUX_PROXY_CONNECT) {
        rcp_proxy_retry(p, why);
        return;
    }
    if (p->fd >= 0) {
        close(p->fd);
        p->fd = -1;
    }
    ESP_LOGI(TAG, "%s: client %s: %s", p->id, p->peer, why);
    rcp_proxy_peer_lost(p, PROXY_LISTENING);
}

// Connect, once p->dst is resolved and allowed.
static esp_err_t tcp_dial(proxy_t *p)
{
    p->fd = socket(p->dst.ss_family, SOCK_STREAM, IPPROTO_TCP);
    if (p->fd < 0) {
        ESP_LOGE(TAG, "socket: errno %d", errno);
        rcp_proxy_note_error(p, "tcp socket: %s", strerror(errno));
        return ESP_FAIL;
    }
    peer_setup(p->fd);
    if (connect(p->fd, (struct sockaddr *)&p->dst, p->dst_len) == 0) {
        rcp_proxy_peer_up(p, (struct sockaddr *)&p->dst, true);
        return ESP_OK;
    }
    if (errno != EINPROGRESS) {
        const int err = errno;

        ESP_LOGW(TAG, "%s: connect to %s:%u: errno %d", p->id, p->set.host, p->set.port, err);
        rcp_proxy_note_error(p, "connect: %s", strerror(err));
        return ESP_FAIL;
    }
    p->conn_deadline_ms = rcp_proxy_now_ms() + CONNECT_TIMEOUT_MS;
    return ESP_OK;
}

static esp_err_t tcp_open(proxy_t *p)
{
    p->lfd = -1;
    p->fd = -1;
    if (p->set.dir == RADIO_MUX_PROXY_LISTEN) {
        p->lfd = listen_on(p, p->set.port);
        if (p->lfd < 0) {
            return ESP_FAIL;
        }
        p->state = PROXY_LISTENING;
        ESP_LOGI(TAG, "%s: listening on TCP port %u", p->id, p->set.port);
        return ESP_OK;
    }
    p->state = PROXY_CONNECTING;
    const esp_err_t err = rcp_proxy_resolve(p, SOCK_STREAM); // a literal at once; a name: the resolver task answers

    if (err == ESP_ERR_NOT_FINISHED) {
        return ESP_OK; // waits for the answer; proxy.c calls tcp_dial then
    }
    return err == ESP_OK ? tcp_dial(p) : err;
}

static void tcp_close(proxy_t *p)
{
    p->sniffing = false;
    if (p->fd >= 0) {
        close(p->fd);
        p->fd = -1;
    }
    if (p->lfd >= 0) {
        close(p->lfd);
        p->lfd = -1;
    }
}

static int add(int fd, fd_set *set, int maxfd)
{
    FD_SET(fd, set);
    return fd > maxfd ? fd : maxfd;
}

static int tcp_fds(proxy_t *p, fd_set *rd, fd_set *wr, int maxfd)
{
    if (p->lfd >= 0) {
        maxfd = add(p->lfd, rd, maxfd);
    }
    if (connecting(p)) {
        maxfd = add(p->fd, wr, maxfd); // a non-blocking connect completes as writable
        p->wake_ms = p->conn_deadline_ms;
    } else if (p->fd >= 0) {
        maxfd = add(p->fd, rd, maxfd);
        if (p->sniffing && p->hold.deadline_ms >= 0) {
            p->wake_ms = p->hold.deadline_ms;
        }
        if (p->ring.len > 0) {
            maxfd = add(p->fd, wr, maxfd);
        }
    }
    return maxfd;
}

// Sends what the ring holds without blocking; what the socket does not take stays at the head of the ring.
static void flush(proxy_t *p)
{
    for (int run = 0; run < 2 && p->fd >= 0; run++) { // the ring's head run, then the wrapped part
        const uint8_t *d;
        const size_t n = rcp_proxy_ring_peek(&p->ring, &d);

        if (n == 0) {
            return;
        }
        const ssize_t r = send(p->fd, d, n, MSG_DONTWAIT);

        if (r < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                lose_peer(p, "send failed");
            }
            return; // full send buffer: the bytes stay in the ring
        }
        rcp_proxy_ring_drop(&p->ring, (size_t)r);
        if ((size_t)r < n) {
            return;
        }
    }
}

static void connect_done(proxy_t *p)
{
    int err = 0;
    socklen_t len = sizeof(err);

    if (getsockopt(p->fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err != 0) {
        char why[RADIO_MUX_PROXY_ERROR_MAX];

        snprintf(why, sizeof(why), "connect: %s", err ? strerror(err) : "failed");
        rcp_proxy_retry(p, why);
        return;
    }
    rcp_proxy_peer_up(p, (struct sockaddr *)&p->dst, true); // every connect is a new peer session
}

// Acts on the hold's decision (rcp_proxy_hold_t). UP: a silent client becomes the peer (the C6 reset replay), its bytes
// still held. PASS: a radio client becomes the peer (if it is not yet) and its held bytes go to the radio. REFUSE: an
// HTTP request is closed (refused + 1) without a byte to the radio; a client that was already the peer is lost.
static void hold_act(proxy_t *p, rcp_proxy_hold_act_t a)
{
    switch (a) {
    case RCP_PROXY_HOLD_WAIT:
        return;
    case RCP_PROXY_HOLD_UP:
        rcp_proxy_peer_up(p, (struct sockaddr *)&p->sniff_from, true); // every accept is a new peer session
        return;
    case RCP_PROXY_HOLD_REFUSE: {
        const int64_t now = rcp_proxy_now_ms();
        const bool was_peer = p->state == PROXY_CONNECTED;

        p->sniffing = false;
        refuse(p->fd);
        p->fd = -1;
        p->refused++;
        if (p->http_log_ms == 0 || now - p->http_log_ms >= HTTP_LOG_MS) {
            p->http_log_ms = now;
            ESP_LOGW(TAG, "%s: an HTTP request (a web page?) refused (%lu refused so far)", p->id,
                     (unsigned long)p->refused);
        }
        if (was_peer) {
            rcp_proxy_peer_lost(p, PROXY_LISTENING);
        }
        return;
    }
    case RCP_PROXY_HOLD_PASS:
        p->sniffing = false;
        if (p->state != PROXY_CONNECTED) {
            // The replayed reset is only posted to the radio's owner task, and the held bytes follow at once, so on
            // the C6 a spinel host's first frame can meet the radio while it resets and be lost. Harmless: the host
            // retries its RESET and gets the replayed reset notification.
            rcp_proxy_peer_up(p, (struct sockaddr *)&p->sniff_from, true); // every accept is a new peer session
        }
        rcp_proxy_from_peer(p, p->hold.buf, p->hold.len); // may drop the peer (a raw write that failed)
        return;
    }
}

static void read_sniff(proxy_t *p)
{
    uint8_t *buf = rcp_proxy_rx_buf();
    const int n = recv(p->fd, buf, rcp_proxy_hold_room(&p->hold), MSG_DONTWAIT);

    if (n > 0) {
        hold_act(p, rcp_proxy_hold_add(&p->hold, buf, (size_t)n, rcp_proxy_now_ms()));
    } else if (n == 0) {
        lose_peer(p, "closed before it sent enough to judge");
    } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
        lose_peer(p, "receive failed");
    }
}

static void read_peer(proxy_t *p)
{
    if (p->sniffing) {
        read_sniff(p);
        return;
    }
    uint8_t *buf = rcp_proxy_rx_buf();
    const int n = recv(p->fd, buf, RECV_CHUNK, MSG_DONTWAIT);

    if (n > 0) {
        rcp_proxy_from_peer(p, buf, (size_t)n);
        return;
    }
    if (n == 0) {
        lose_peer(p, "closed by the peer");
    } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
        lose_peer(p, "receive failed");
    }
}

static void accept_one(proxy_t *p)
{
    struct sockaddr_storage sa;
    socklen_t len = sizeof(sa);
    const int fd = accept(p->lfd, (struct sockaddr *)&sa, &len);

    if (fd < 0) {
        return;
    }
    // Both access rules: the local address the client came in on, and the route back to it (a Thread mesh node that
    // dials an infrastructure address passes the first and is refused by the second).
    const bool busy = p->fd >= 0;
    const bool local_ok = !busy && netcfg_socket_allowed(fd, NETCFG_SVC_RCP_PROXY);
    const bool peer_ok = local_ok &&
                         netcfg_peer_allowed_as((const struct sockaddr *)&sa, NETCFG_SVC_RCP_PROXY, "a client");

    if (rcp_proxy_accept(busy, local_ok, peer_ok) == RCP_PROXY_REFUSE) {
        refuse(fd);
        p->refused++;
        ESP_LOGI(TAG, "%s: connection refused (%lu so far)", p->id, (unsigned long)p->refused);
        return;
    }
    peer_setup(fd);
    p->fd = fd;
    p->sniffing = true; // the peer once its first bytes pass, or once it was silent long enough (hold_act)
    rcp_proxy_hold_start(&p->hold, rcp_proxy_now_ms());
    p->sniff_from = sa;
}

static void tcp_ready(proxy_t *p, fd_set *rd, fd_set *wr)
{
    if (connecting(p)) {
        if (FD_ISSET(p->fd, wr)) {
            connect_done(p);
        } else if (rcp_proxy_now_ms() >= p->conn_deadline_ms) {
            rcp_proxy_retry(p, "connect timed out");
        }
        return;
    }
    // The old client first: a client that closes and reconnects at once makes both readable, and its close must be
    // seen before the new connection is judged.
    if (p->fd >= 0 && FD_ISSET(p->fd, rd)) {
        read_peer(p);
    }
    if (p->sniffing) {
        hold_act(p, rcp_proxy_hold_tick(&p->hold, rcp_proxy_now_ms()));
    }
    if (p->fd >= 0 && FD_ISSET(p->fd, wr)) {
        flush(p);
    }
    if (p->lfd >= 0 && FD_ISSET(p->lfd, rd)) {
        accept_one(p);
    }
}

static esp_err_t tcp_send(proxy_t *p, const uint8_t *d, size_t n)
{
    if (p->fd < 0 || p->state != PROXY_CONNECTED) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!rcp_proxy_ring_push(&p->ring, d, n)) {
        p->overflows++;
        ESP_LOGW(TAG, "%s: the peer cannot keep up with the radio: disconnected (%lu so far)", p->id,
                 (unsigned long)p->overflows);
        lose_peer(p, "overflow");
        return ESP_ERR_NO_MEM;
    }
    flush(p);
    return ESP_OK;
}

static void tcp_drop(proxy_t *p, bool cut)
{
    if (p->fd >= 0 && (p->state == PROXY_CONNECTED || p->sniffing)) {
        lose_peer(p, cut ? "dropped: a radio write was cut" : "kicked"); // a new client starts a new stream
    }
}

const rcp_proxy_adapter_t rcp_proxy_adapter_tcp = {
    .name = "tcp",
    .datagram = false,
    .open = tcp_open,
    .dial = tcp_dial,
    .close = tcp_close,
    .fds = tcp_fds,
    .ready = tcp_ready,
    .send = tcp_send,
    .drop = tcp_drop,
};
