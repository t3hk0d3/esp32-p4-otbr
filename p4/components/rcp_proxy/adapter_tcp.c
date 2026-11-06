// TCP adapter (from usb_serial's tcp.c): listen (first client wins, refusals reset with SO_LINGER {1, 0}) or connect
// (non-blocking dial to a target the netcfg rules allow, back-off on failure). Clients get TCP_NODELAY and keepalive
// 10/2/3; every accept and connect is a new peer session (reset replay on a frame channel). Bytes towards the peer
// wait in the session's ring while the socket has no room; a ring that would overflow disconnects the peer.
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "esp_log.h"
#include "lwip/sockets.h"
#include "netcfg.h"
#include "rcp_proxy_priv.h"
#include "sdkconfig.h"

#if !CONFIG_LWIP_SO_LINGER
#error "CONFIG_LWIP_SO_LINGER is off: a refused connection would not be reset; enable it in p4/sdkconfig.defaults, else p4/sdkconfig predates it: delete p4/sdkconfig and build again (docs/development.md)."
#endif

static const char *TAG = "rcp_proxy";

#define LISTEN_BACKLOG 1
#define KEEP_IDLE_S 10
#define KEEP_INTVL_S 2
#define KEEP_COUNT 3
#define CONNECT_TIMEOUT_MS 10000
#define RECV_CHUNK 1024

static bool connecting(const proxy_t *p)
{
    return p->state == PROXY_CONNECTING && p->fd >= 0;
}

static int listen_on(uint16_t port)
{
    const int fd = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
    const int one = 1;
    const int zero = 0;

    if (fd < 0) {
        ESP_LOGE(TAG, "socket: errno %d", errno);
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
        ESP_LOGE(TAG, "listening on TCP port %u: errno %d", port, errno);
        close(fd);
        return -1;
    }
    rcp_proxy_set_nonblocking(fd);
    return fd;
}

// Closes with SO_LINGER {1, 0}: an RST, no TIME_WAIT.
static void refuse(int fd)
{
    const struct linger lg = {.l_onoff = 1, .l_linger = 0};

    setsockopt(fd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
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

static esp_err_t tcp_open(proxy_t *p)
{
    p->lfd = -1;
    p->fd = -1;
    if (p->set.dir == RADIO_MUX_PROXY_LISTEN) {
        p->lfd = listen_on(p->set.port);
        if (p->lfd < 0) {
            return ESP_FAIL;
        }
        p->state = PROXY_LISTENING;
        ESP_LOGI(TAG, "%s: listening on TCP port %u", p->id, p->set.port);
        return ESP_OK;
    }
    p->state = PROXY_CONNECTING;
    const esp_err_t err = rcp_proxy_resolve(p, SOCK_STREAM); // resolves and applies netcfg_peer_allowed

    if (err != ESP_OK) {
        return err;
    }
    p->fd = socket(p->dst.ss_family, SOCK_STREAM, IPPROTO_TCP);
    if (p->fd < 0) {
        ESP_LOGE(TAG, "socket: errno %d", errno);
        return ESP_FAIL;
    }
    peer_setup(p->fd);
    if (connect(p->fd, (struct sockaddr *)&p->dst, p->dst_len) == 0) {
        rcp_proxy_peer_up(p, (struct sockaddr *)&p->dst, true);
        return ESP_OK;
    }
    if (errno != EINPROGRESS) {
        ESP_LOGW(TAG, "%s: connect to %s:%u: errno %d", p->id, p->set.host, p->set.port, errno);
        return ESP_FAIL;
    }
    p->conn_deadline_ms = rcp_proxy_now_ms() + CONNECT_TIMEOUT_MS;
    return ESP_OK;
}

static void tcp_close(proxy_t *p)
{
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
        char why[48];

        snprintf(why, sizeof(why), "connect failed (errno %d)", err);
        rcp_proxy_retry(p, why);
        return;
    }
    rcp_proxy_peer_up(p, (struct sockaddr *)&p->dst, true); // every connect is a new peer session
}

static void read_peer(proxy_t *p)
{
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
    if (rcp_proxy_accept(p->fd >= 0) == RCP_PROXY_REFUSE || !netcfg_socket_allowed(fd, NETCFG_SVC_RCP_PROXY)) {
        refuse(fd);
        p->refused++;
        ESP_LOGI(TAG, "%s: connection refused (%lu so far)", p->id, (unsigned long)p->refused);
        return;
    }
    peer_setup(fd);
    p->fd = fd;
    rcp_proxy_peer_up(p, (struct sockaddr *)&sa, true); // every accept is a new peer session
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

static void tcp_drop(proxy_t *p)
{
    if (p->fd >= 0 && p->state == PROXY_CONNECTED) {
        lose_peer(p, "kicked");
    }
}

const rcp_proxy_adapter_t rcp_proxy_adapter_tcp = {
    .name = "tcp",
    .datagram = false,
    .open = tcp_open,
    .close = tcp_close,
    .fds = tcp_fds,
    .ready = tcp_ready,
    .send = tcp_send,
    .drop = tcp_drop,
};
