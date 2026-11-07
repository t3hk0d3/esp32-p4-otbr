// UDP adapter: one HDLC frame (framing hdlc) or up to 1024 bytes (framing raw) per datagram; proxy.c cuts and encodes.
// Listen: the first allowed source becomes the peer until 30 s of silence or a kick (rcp_proxy_udp_lock); the netcfg
// access rules are checked for a source before it can lock. Connect: datagrams go to host:port (a target the netcfg
// rules allow), and only datagrams from that address are taken; the state is connected from its first datagram until
// 30 s of silence. The reset replay runs for a new peer session only (rcp_proxy_peer_new): once per socket session in
// connect, and in listen when the locked source differs from the previous peer.
#include <errno.h>
#include <string.h>
#include <unistd.h>

#include "esp_log.h"
#include "lwip/sockets.h"
#include "netcfg.h"
#include "rcp_proxy_priv.h"

static const char *TAG = "rcp_proxy";

// Datagrams taken per wake: a burst does not starve the other channel; the rest stay readable for the next wait.
#define DATAGRAMS_PER_WAKE 16

static bool is_listen(const proxy_t *p)
{
    return p->set.dir == RADIO_MUX_PROXY_LISTEN;
}

// Without a peer: listen waits for a source, connect for the target's first datagram.
static proxy_state_t waiting_state(const proxy_t *p)
{
    return is_listen(p) ? PROXY_LISTENING : PROXY_CONNECTING;
}

// A socket bound to `port`; -1 with the reason in p's last_error.
static int bind_listen(proxy_t *p, uint16_t port)
{
    const int fd = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
    const int one = 1;
    const int zero = 0;

    if (fd < 0) {
        ESP_LOGE(TAG, "socket: errno %d", errno);
        rcp_proxy_note_error(p, "udp socket: %s", strerror(errno));
        return -1;
    }
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof(zero)); // dual-stack: IPv4 sources too
    struct sockaddr_in6 sa = {
        .sin6_family = AF_INET6,
        .sin6_port = htons(port),
        .sin6_addr = IN6ADDR_ANY_INIT,
    };

    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        const int err = errno;

        ESP_LOGE(TAG, "binding UDP port %u: errno %d", port, err);
        rcp_proxy_note_error(p, "udp port %u: %s", port, strerror(err));
        close(fd);
        return -1;
    }
    rcp_proxy_set_nonblocking(fd);
    return fd;
}

// Connect, once p->dst is resolved and allowed.
static esp_err_t udp_dial(proxy_t *p)
{
    p->fd = socket(p->dst.ss_family, SOCK_DGRAM, IPPROTO_UDP);
    if (p->fd < 0) {
        ESP_LOGE(TAG, "socket: errno %d", errno);
        rcp_proxy_note_error(p, "udp socket: %s", strerror(errno));
        return ESP_FAIL;
    }
    rcp_proxy_set_nonblocking(p->fd);
    ESP_LOGI(TAG, "%s: sending UDP to %s:%u", p->id, p->set.host, p->set.port);
    return ESP_OK;
}

static esp_err_t udp_open(proxy_t *p)
{
    p->fd = -1;
    rcp_proxy_udp_lock_init(&p->lock);
    rcp_proxy_peer_memo_init(&p->memo); // a new socket session: the next peer is new
    p->state = waiting_state(p);
    if (is_listen(p)) {
        p->fd = bind_listen(p, p->set.port);
        if (p->fd < 0) {
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "%s: listening on UDP port %u", p->id, p->set.port);
        return ESP_OK;
    }
    const esp_err_t err = rcp_proxy_resolve(p, SOCK_DGRAM); // a literal at once; a name: the resolver task answers

    if (err == ESP_ERR_NOT_FINISHED) {
        return ESP_OK; // waits for the answer; proxy.c calls udp_dial then
    }
    return err == ESP_OK ? udp_dial(p) : err;
}

static void udp_close(proxy_t *p)
{
    if (p->fd >= 0) {
        close(p->fd);
        p->fd = -1;
    }
    rcp_proxy_udp_lock_kick(&p->lock);
}

static int udp_fds(proxy_t *p, fd_set *rd, fd_set *wr, int maxfd)
{
    if (p->fd < 0) {
        return maxfd;
    }
    FD_SET(p->fd, rd);
    p->wake_ms = rcp_proxy_udp_lock_deadline(&p->lock);
    return p->fd > maxfd ? p->fd : maxfd;
}

static void let_go(proxy_t *p, const char *why)
{
    ESP_LOGI(TAG, "%s: peer %s %s", p->id, p->peer, why);
    rcp_proxy_udp_lock_kick(&p->lock);
    rcp_proxy_peer_lost(p, waiting_state(p));
}

// Whether a datagram from src is the peer's; a first one (or the first after a silence) makes it the peer.
static bool take_source(proxy_t *p, const struct sockaddr_storage *src, socklen_t len, int64_t now)
{
    rcp_proxy_addr_t a;
    const bool was_locked = p->lock.locked;

    rcp_proxy_addr_from((const struct sockaddr *)src, &a);
    if (!is_listen(p)) { // only the configured target, whatever zone the reply names
        rcp_proxy_addr_t want;

        rcp_proxy_addr_from((const struct sockaddr *)&p->dst, &want);
        a.scope = want.scope = 0;
        if (!rcp_proxy_addr_eq(&a, &want)) {
            p->refused++;
            return false;
        }
    } else if (!p->lock.locked && !netcfg_peer_allowed((const struct sockaddr *)src, NETCFG_SVC_RCP_PROXY)) {
        p->refused++;
        return false;
    }
    if (!rcp_proxy_udp_lock_offer(&p->lock, &a, now)) {
        p->refused++;
        return false;
    }
    if (!was_locked) {
        if (is_listen(p)) { // the peer the radio's datagrams go to
            p->dst = *src;
            p->dst_len = len;
        }
        rcp_proxy_peer_up(p, (const struct sockaddr *)src, rcp_proxy_peer_new(&p->memo, &a));
    }
    return true;
}

static void udp_ready(proxy_t *p, fd_set *rd, fd_set *wr)
{
    const int64_t now = rcp_proxy_now_ms();

    if (rcp_proxy_udp_lock_expire(&p->lock, now)) {
        let_go(p, "silent for 30 s");
    }
    if (p->fd < 0 || !FD_ISSET(p->fd, rd)) {
        return;
    }
    uint8_t *buf = rcp_proxy_rx_buf();

    for (int i = 0; i < DATAGRAMS_PER_WAKE && p->fd >= 0; i++) {
        struct sockaddr_storage src;
        socklen_t len = sizeof(src);
        const int n = recvfrom(p->fd, buf, RCP_PROXY_RX_MAX, MSG_DONTWAIT, (struct sockaddr *)&src, &len);

        if (n < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                rcp_proxy_retry(p, "receive failed");
            }
            return;
        }
        if (n > 0 && take_source(p, &src, len, now)) {
            rcp_proxy_from_peer(p, buf, (size_t)n);
        }
    }
}

static esp_err_t udp_send(proxy_t *p, const uint8_t *d, size_t n)
{
    if (p->fd < 0 || (is_listen(p) && !p->lock.locked)) {
        return ESP_ERR_INVALID_STATE; // listen: nobody to send to yet
    }
    if (sendto(p->fd, d, n, 0, (struct sockaddr *)&p->dst, p->dst_len) < 0) {
        return ESP_FAIL; // counted as discarded; a full lwIP queue drops the datagram, as the network would
    }
    return ESP_OK;
}

static void udp_drop(proxy_t *p)
{
    if (p->lock.locked) {
        let_go(p, "kicked");
    }
}

const rcp_proxy_adapter_t rcp_proxy_adapter_udp = {
    .name = "udp",
    .datagram = true,
    .open = udp_open,
    .dial = udp_dial,
    .close = udp_close,
    .fds = udp_fds,
    .ready = udp_ready,
    .send = udp_send,
    .drop = udp_drop,
};
