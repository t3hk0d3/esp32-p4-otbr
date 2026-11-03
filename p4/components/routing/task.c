// Routing task (target only): the sockets, the clock, the OpenThread lock and FreeRTOS around the engine (engine.h),
// which makes every decision. Every socket is bound to the backbone netif (SO_BINDTODEVICE); as a second guard an
// accepted connection whose local address is not on the backbone is closed, and RIPng datagrams whose source is
// neither link-local on the backbone nor on-link there are dropped, so nothing is sent or answered on mgmt.
#include "routing.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/param.h>

#include "board_cores.h"
#include "br_core.h"
#include "engine.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_openthread.h"
#include "esp_openthread_lock.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "netcfg.h"
#include "openthread/border_routing.h"
#include "openthread/thread.h"
#include "publisher.h"

static const char *TAG = "routing";

#define ROUTING_TASK_STACK 6144
#define ROUTING_TASK_PRIORITY (tskIDLE_PRIORITY + 2)
#define ROUTING_TASK_CORE MGMT_CORE
#define STATE_PERIOD_MS 500
#define OT_LOCK_POLL_MS 10   /* a busy lock skips that state read (as the metrics collector does) */
#define OT_LOCK_APPLY_MS 100 /* settings changes and validation wait a little longer; a timeout is retried */
#define WAKE_MAX_MS 100      /* how long a new setting can wait in the mailbox */
#define LIVE_CONNS (2 * ROUTING_MAX_PEERS)              /* a current and a pending connection per peer */
#define MAX_CONNS (LIVE_CONNS + ROUTING_MAX_PEERS)      /* plus connections closing after a NOTIFICATION */
#define LINGER_MS 1000 /* how long a connection closing after a NOTIFICATION waits for the peer's FIN */
#define DRAIN_CHUNK 256
#define DRAIN_READS 8
#define STATE_RETRY_MS 100 /* a skipped state read is retried this soon, so withdrawal stays within 1 s */
#define LISTEN_BACKLOG 2
#define RIPNG_RX_MAX 1500
#define RIPNG_RX_PER_WAKE 16
#define HOP_LIMIT 255
#define IFNAME_MAX 8

typedef struct {
    esp_netif_t *netif;
    int index; /**< lwIP netif index, the zone of link-local addresses */
    char name[IFNAME_MAX];
} backbone_t;

typedef struct {
    int fd; /**< -1 unused */
    bool connecting;
    bool notified;  /**< the last message sent was a NOTIFICATION: close gracefully */
    bool lingering; /**< closed by the engine, write side shut, waiting for the peer's FIN until linger_until */
    uint32_t linger_until;
} conn_slot_t;

static routing_engine_t s_engine;
static bool s_started;
static QueueHandle_t s_mailbox;   /* one routing_settings_t, the latest wins */
static SemaphoreHandle_t s_mutex; /* s_settings, s_settings_gen, s_publisher_ready, s_status, s_view */
static routing_settings_t s_settings; /* latest stored */
static uint32_t s_settings_gen;       /* bumped by every routing_apply */
static routing_status_t s_status;
static routing_metrics_view_t s_view;

/* only under the OpenThread lock */
static routing_publisher_t s_publisher;
/*
 * Set by the border-router-up hook once it applied the latest settings, with both the OpenThread lock and s_mutex
 * held: readers take either one.
 */
static bool s_publisher_ready;

/* routing task only */
static int s_listen_fd = -1;
static int s_ripng_fd = -1;
static conn_slot_t s_conns[MAX_CONNS];
static uint8_t s_ripng_rx[RIPNG_RX_MAX];
static bool s_ula_known; /* the last ULA read, kept for the status while OpenThread is not readable */
static routing_prefix64_t s_ula;
static bool s_bind_warned;
static routing_bgp_state_t s_logged_state[ROUTING_MAX_PEERS];

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static bool is_link_local(const uint8_t a[16])
{
    return a[0] == 0xfe && (a[1] & 0xc0) == 0x80;
}

static bool is_ula(const uint8_t a[16])
{
    return (a[0] & 0xfe) == 0xfc;
}

/* --- backbone --- */

static bool get_backbone(backbone_t *bb)
{
    memset(bb, 0, sizeof(*bb));
    bb->netif = netcfg_backbone_netif();
    if (!bb->netif) {
        return false;
    }
    bb->index = esp_netif_get_netif_impl_index(bb->netif);
    if (bb->index < 0 || esp_netif_get_netif_impl_name(bb->netif, bb->name) != ESP_OK) {
        return false;
    }
    return true;
}

static int netif_ip6(esp_netif_t *netif, uint8_t out[][16], int cap)
{
    esp_ip6_addr_t addrs[CONFIG_LWIP_IPV6_NUM_ADDRESSES];
    int n = netif ? esp_netif_get_all_ip6(netif, addrs) : 0;

    n = MIN(n, cap);
    for (int i = 0; i < n; i++) {
        memcpy(out[i], addrs[i].addr, 16);
    }
    return n;
}

/* The backbone addresses for the engine: IPv4 (router ID), link-local and a global next-hop (GUA before ULA). */
static void read_backbone(const backbone_t *bb, routing_engine_state_t *st)
{
    uint8_t addrs[CONFIG_LWIP_IPV6_NUM_ADDRESSES][16];
    int n = netif_ip6(bb->netif, addrs, CONFIG_LWIP_IPV6_NUM_ADDRESSES);
    esp_netif_ip_info_t ip4;

    if (bb->netif && esp_netif_get_ip_info(bb->netif, &ip4) == ESP_OK) {
        st->backbone_ipv4 = ntohl(ip4.ip.addr);
    }
    for (int i = 0; i < n; i++) {
        if (is_link_local(addrs[i])) {
            if (!st->have_link_local) {
                st->have_link_local = true;
                memcpy(st->nexthop.link_local, addrs[i], 16);
            }
        } else if (!st->nexthop.has_global || (is_ula(st->nexthop.global) && !is_ula(addrs[i]))) {
            st->nexthop.has_global = true;
            memcpy(st->nexthop.global, addrs[i], 16);
        }
    }
}

/* `addr` is one of the backbone's own addresses. */
static bool is_backbone_address(const backbone_t *bb, const uint8_t addr[16])
{
    uint8_t addrs[CONFIG_LWIP_IPV6_NUM_ADDRESSES][16];
    int n = netif_ip6(bb->netif, addrs, CONFIG_LWIP_IPV6_NUM_ADDRESSES);

    for (int i = 0; i < n; i++) {
        if (memcmp(addrs[i], addr, 16) == 0) {
            return true;
        }
    }
    return false;
}

/* `addr` is in the /64 of one of the backbone's own global addresses. */
static bool is_backbone_onlink(const backbone_t *bb, const uint8_t addr[16])
{
    uint8_t addrs[CONFIG_LWIP_IPV6_NUM_ADDRESSES][16];
    int n = netif_ip6(bb->netif, addrs, CONFIG_LWIP_IPV6_NUM_ADDRESSES);

    for (int i = 0; i < n; i++) {
        if (!is_link_local(addrs[i]) && memcmp(addrs[i], addr, 8) == 0) {
            return true;
        }
    }
    return false;
}

/* --- sockets --- */

static const char *errno_text(int err)
{
    switch (err) {
    case ECONNREFUSED:
        return "refused";
    case EHOSTUNREACH:
        return "host unreachable";
    case ENETUNREACH:
        return "network unreachable";
    case ETIMEDOUT:
        return "timed out";
    case ECONNRESET:
        return "reset";
    case ENOMEM:
    case ENFILE:
    case EMFILE:
        return "no socket";
    default:
        return "failed";
    }
}

static void bind_to_backbone(int fd, const backbone_t *bb)
{
    struct ifreq ifr;

    memset(&ifr, 0, sizeof(ifr));
    strlcpy(ifr.ifr_name, bb->name, sizeof(ifr.ifr_name));
    if (setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, &ifr, sizeof(ifr)) != 0 && !s_bind_warned) {
        s_bind_warned = true;
        ESP_LOGW(TAG, "SO_BINDTODEVICE %s failed (errno %d): relying on the address checks", bb->name, errno);
    }
}

static void set_nonblocking(int fd)
{
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
}

static void make_sockaddr(struct sockaddr_in6 *sa, const uint8_t addr[16], uint16_t port, const backbone_t *bb)
{
    memset(sa, 0, sizeof(*sa));
    sa->sin6_family = AF_INET6;
    sa->sin6_port = htons(port);
    memcpy(&sa->sin6_addr, addr, 16);
    /* link-local unicast and link-scope multicast live in the backbone's zone */
    if (is_link_local(addr) || (addr[0] == 0xff && (addr[1] & 0x0f) == 0x02)) {
        sa->sin6_scope_id = bb->index;
    }
}

static conn_slot_t *conn_find(int fd)
{
    for (int i = 0; i < MAX_CONNS; i++) {
        if (s_conns[i].fd == fd && fd >= 0) {
            return &s_conns[i];
        }
    }
    return NULL;
}

static void conn_release(conn_slot_t *slot)
{
    close(slot->fd);
    memset(slot, 0, sizeof(*slot));
    slot->fd = -1;
}

/* A free slot; when only closing connections hold the rest, the one closest to its deadline is closed for it. */
static conn_slot_t *conn_free_slot(void)
{
    conn_slot_t *oldest = NULL;

    for (int i = 0; i < MAX_CONNS; i++) {
        if (s_conns[i].fd < 0) {
            return &s_conns[i];
        }
        if (s_conns[i].lingering &&
                (!oldest || (int32_t)(s_conns[i].linger_until - oldest->linger_until) < 0)) {
            oldest = &s_conns[i];
        }
    }
    if (oldest) {
        conn_release(oldest);
    }
    return oldest;
}

/* Room for one more connection the engine owns. */
static bool conn_room(void)
{
    int live = 0;

    for (int i = 0; i < MAX_CONNS; i++) {
        live += s_conns[i].fd >= 0 && !s_conns[i].lingering;
    }
    return live < LIVE_CONNS;
}

/* Callers checked conn_room(). */
static void conn_add(int fd, bool connecting)
{
    conn_slot_t *slot = conn_free_slot();

    if (slot) {
        memset(slot, 0, sizeof(*slot));
        slot->fd = fd;
        slot->connecting = connecting;
    }
}

/* Reads and discards what the peer sent; true when it closed (EOF) or the connection failed. */
static bool conn_drain(int fd)
{
    static uint8_t sink[DRAIN_CHUNK];

    for (int i = 0; i < DRAIN_READS; i++) {
        ssize_t n = recv(fd, sink, sizeof(sink), MSG_DONTWAIT);
        if (n == 0) {
            return true;
        }
        if (n < 0) {
            return errno != EAGAIN && errno != EWOULDBLOCK;
        }
    }
    return false;
}

/* Closes the closing connections whose peer closed or whose deadline passed. */
static void conn_linger_expire(uint32_t now)
{
    for (int i = 0; i < MAX_CONNS; i++) {
        if (s_conns[i].lingering && (int32_t)(now - s_conns[i].linger_until) >= 0) {
            conn_release(&s_conns[i]);
        }
    }
}

/* --- engine I/O --- */

static int io_tcp_connect(void *ctx, const uint8_t addr[16], const char **why)
{
    backbone_t bb;
    struct sockaddr_in6 sa;

    if (!get_backbone(&bb)) {
        *why = "no backbone";
        return -1;
    }
    if (!conn_room()) {
        *why = "no socket";
        return -1;
    }
    int fd = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        *why = errno_text(errno);
        return -1;
    }
    bind_to_backbone(fd, &bb);
    set_nonblocking(fd);
    make_sockaddr(&sa, addr, ROUTING_BGP_PORT, &bb);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0 && errno != EINPROGRESS) {
        *why = errno_text(errno);
        close(fd);
        return -1;
    }
    conn_add(fd, true); /* writable once connected (or failed), also when connect returned 0 */
    return fd;
}

static int io_tcp_send(void *ctx, int h, const uint8_t *buf, size_t len)
{
    /* BGP messages are small next to the send buffer: a short write means the peer stopped reading */
    ssize_t n = send(h, buf, len, 0);
    conn_slot_t *slot = conn_find(h);

    if (n != (ssize_t)len) {
        return -1;
    }
    if (slot) {
        slot->notified = len >= ROUTING_BGP_HDR_LEN && buf[18] == ROUTING_BGP_MSG_NOTIFICATION;
    }
    return 0;
}

/*
 * After a NOTIFICATION, close() with unread received data would make lwIP send RST and drop the queued NOTIFICATION
 * (tcp_close_shutdown). So: drain what arrived, shut the write side (the NOTIFICATION goes out, then FIN), and close
 * once the peer closes or LINGER_MS has passed. The slot stays reserved until then.
 */
static void io_tcp_close(void *ctx, int h)
{
    conn_slot_t *slot = conn_find(h);

    if (!slot) {
        close(h);
        return;
    }
    if (!slot->notified || slot->connecting || conn_drain(h)) {
        conn_release(slot);
        return;
    }
    shutdown(h, SHUT_WR);
    slot->lingering = true;
    slot->linger_until = now_ms() + LINGER_MS;
}

static bool io_bgp_listen(void *ctx, bool on)
{
    backbone_t bb;
    struct sockaddr_in6 sa;
    int one = 1;

    if (!on) {
        if (s_listen_fd >= 0) {
            close(s_listen_fd);
            s_listen_fd = -1;
        }
        return true;
    }
    if (s_listen_fd >= 0) {
        return true;
    }
    if (!get_backbone(&bb)) {
        return false;
    }
    int fd = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        ESP_LOGW(TAG, "BGP listener: no socket (errno %d)", errno);
        return false;
    }
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &one, sizeof(one));
    bind_to_backbone(fd, &bb);
    memset(&sa, 0, sizeof(sa));
    sa.sin6_family = AF_INET6;
    sa.sin6_port = htons(ROUTING_BGP_PORT);
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0 || listen(fd, LISTEN_BACKLOG) != 0) {
        ESP_LOGW(TAG, "BGP listener on port %d failed (errno %d)", ROUTING_BGP_PORT, errno);
        close(fd);
        return false;
    }
    set_nonblocking(fd);
    s_listen_fd = fd;
    return true;
}

static bool io_ripng_socket(void *ctx, bool on)
{
    backbone_t bb;
    struct sockaddr_in6 sa;
    int one = 1;
    int ttl = HOP_LIMIT;
    uint8_t hops = HOP_LIMIT;
    uint8_t loop = 0;
    struct ipv6_mreq mreq;
    static const uint8_t group[16] = {0xff, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x09};

    if (!on) {
        if (s_ripng_fd >= 0) {
            close(s_ripng_fd);
            s_ripng_fd = -1;
        }
        return true;
    }
    if (s_ripng_fd >= 0) {
        return true;
    }
    if (!get_backbone(&bb)) {
        return false;
    }
    int fd = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        ESP_LOGW(TAG, "RIPng: no socket (errno %d)", errno);
        return false;
    }
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    bind_to_backbone(fd, &bb);
    memset(&sa, 0, sizeof(sa));
    sa.sin6_family = AF_INET6;
    sa.sin6_port = htons(ROUTING_RIPNG_PORT);
    uint8_t index = (uint8_t)bb.index;
    memset(&mreq, 0, sizeof(mreq));
    memcpy(&mreq.ipv6mr_multiaddr, group, sizeof(group));
    mreq.ipv6mr_interface = bb.index;
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0 ||
            setsockopt(fd, IPPROTO_IPV6, IPV6_MULTICAST_IF, &index, sizeof(index)) != 0 ||
            setsockopt(fd, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, &hops, sizeof(hops)) != 0 ||
            setsockopt(fd, IPPROTO_IPV6, IPV6_JOIN_GROUP, &mreq, sizeof(mreq)) != 0) {
        ESP_LOGW(TAG, "RIPng socket on %s failed (errno %d)", bb.name, errno);
        close(fd);
        return false;
    }
    setsockopt(fd, IPPROTO_IPV6, IPV6_MULTICAST_LOOP, &loop, sizeof(loop));
    setsockopt(fd, IPPROTO_IP, IP_TTL, &ttl, sizeof(ttl)); /* lwIP: the hop limit of unicast answers */
    set_nonblocking(fd);
    s_ripng_fd = fd;
    return true;
}

static int io_ripng_send(void *ctx, const uint8_t *dst, uint16_t port, const uint8_t *buf, size_t len)
{
    static const uint8_t all_rip_routers[16] = {0xff, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x09};
    backbone_t bb;
    struct sockaddr_in6 sa;

    if (s_ripng_fd < 0 || !get_backbone(&bb)) {
        return -1;
    }
    make_sockaddr(&sa, dst ? dst : all_rip_routers, dst ? port : ROUTING_RIPNG_PORT, &bb);
    ssize_t n = sendto(s_ripng_fd, buf, len, 0, (struct sockaddr *)&sa, sizeof(sa));
    return n == (ssize_t)len ? 0 : -1;
}

static bool io_publisher_apply(void *ctx, const routing_settings_t *s)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool ready = s_publisher_ready;
    xSemaphoreGive(s_mutex);
    if (!ready) {
        /* the hook has not finished: it applies s_settings and re-reads them if a PUT changed them meanwhile */
        return true;
    }
    if (br_core_get_mode() != BR_MODE_BR_RUNNING ||
            !esp_openthread_lock_acquire(pdMS_TO_TICKS(OT_LOCK_APPLY_MS))) {
        return false;
    }
    bool ok = br_core_get_mode() == BR_MODE_BR_RUNNING;
    if (ok) {
        routing_publisher_apply(&s_publisher, s);
    }
    esp_openthread_lock_release();
    return ok;
}

static uint32_t io_rand32(void)
{
    return esp_random();
}

static const routing_engine_io_t s_io = {
    .ctx = NULL,
    .tcp_connect = io_tcp_connect,
    .tcp_send = io_tcp_send,
    .tcp_close = io_tcp_close,
    .bgp_listen = io_bgp_listen,
    .ripng_socket = io_ripng_socket,
    .ripng_send = io_ripng_send,
    .publisher_apply = io_publisher_apply,
    .rand32 = io_rand32,
};

/* --- OpenThread state --- */

static void read_ot(otInstance *ins, routing_engine_state_t *st)
{
    otDeviceRole role = otThreadGetDeviceRole(ins);
    otBorderRoutingPrefixTableEntry pd;
    const routing_settings_t *s = routing_engine_settings(&s_engine);

    st->br_up = role == OT_DEVICE_ROLE_CHILD || role == OT_DEVICE_ROLE_ROUTER || role == OT_DEVICE_ROLE_LEADER;
    if (s_publisher_ready) {
        routing_announce_input_t in;
        memset(&in, 0, sizeof(in));
        routing_publisher_sync_default_route(&s_publisher);
        routing_publisher_prefix_state(&s_publisher, &in);
        st->ula_known = in.ula_known;
        st->ula = in.ula;
        st->ula_in_netdata = in.ula_in_netdata;
    }
    /* for the engine's own custom prefix, which may be newer than the one the publisher applied last */
    if (s->omr_mode == ROUTING_OMR_CUSTOM) {
        st->custom_in_netdata = routing_ot_ops_openthread.netdata_has_prefix(&s->omr_prefix);
    }
    if (otBorderRoutingGetPdOmrPrefix(ins, &pd) == OT_ERROR_NONE && pd.mPrefix.mLength == 64) {
        st->pd_known = true;
        memcpy(st->pd_prefix.b, pd.mPrefix.mPrefix.mFields.m8, sizeof(st->pd_prefix.b));
    }
}

/* Reads the state and polls the engine; false (nothing done) when the OpenThread lock was busy. */
static bool poll_state(uint32_t now)
{
    routing_engine_state_t st;
    backbone_t bb;

    memset(&st, 0, sizeof(st));
    if (br_core_get_mode() == BR_MODE_BR_RUNNING) {
        if (!esp_openthread_lock_acquire(pdMS_TO_TICKS(OT_LOCK_POLL_MS))) {
            return false;
        }
        otInstance *ins = esp_openthread_get_instance();
        /* the mode may have changed while waiting */
        if (ins && br_core_get_mode() == BR_MODE_BR_RUNNING) {
            read_ot(ins, &st);
        }
        esp_openthread_lock_release();
    }
    if (st.ula_known) {
        s_ula_known = true;
        s_ula = st.ula;
    } else if (s_ula_known) {
        st.ula_known = true; /* for the status only: br_up is false, nothing is announced */
        st.ula = s_ula;
    }
    if (get_backbone(&bb)) {
        read_backbone(&bb, &st);
    }
    routing_engine_poll(&s_engine, &st, now);
    return true;
}

/* --- socket events --- */

static void accept_one(uint32_t now)
{
    struct sockaddr_in6 peer;
    struct sockaddr_in6 local;
    socklen_t len = sizeof(peer);
    socklen_t local_len = sizeof(local);
    backbone_t bb;

    int fd = accept(s_listen_fd, (struct sockaddr *)&peer, &len);
    if (fd < 0) {
        return;
    }
    /* second guard next to SO_BINDTODEVICE: the connection must have arrived on a backbone address */
    if (!get_backbone(&bb) || getsockname(fd, (struct sockaddr *)&local, &local_len) != 0 ||
            !is_backbone_address(&bb, (const uint8_t *)&local.sin6_addr) || !conn_room()) {
        close(fd);
        return;
    }
    set_nonblocking(fd);
    conn_add(fd, false);
    routing_engine_tcp_accepted(&s_engine, fd, (const uint8_t *)&peer.sin6_addr, now);
}

static void finish_connect(conn_slot_t *slot, uint32_t now)
{
    int err = 0;
    socklen_t len = sizeof(err);
    int fd = slot->fd;

    struct sockaddr_in6 local;
    socklen_t local_len = sizeof(local);
    backbone_t bb;

    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0) {
        err = errno;
    }
    slot->connecting = false;
    if (err != 0) {
        routing_engine_tcp_connected(&s_engine, fd, false, errno_text(err), now);
        return;
    }
    /* second guard next to SO_BINDTODEVICE: the connection must have left from a backbone address */
    if (!get_backbone(&bb) || getsockname(fd, (struct sockaddr *)&local, &local_len) != 0 ||
            !is_backbone_address(&bb, (const uint8_t *)&local.sin6_addr)) {
        routing_engine_tcp_connected(&s_engine, fd, false, "not on the backbone", now);
        return;
    }
    routing_engine_tcp_connected(&s_engine, fd, true, NULL, now);
}

static void receive_tcp(int fd, uint32_t now)
{
    uint8_t *buf = NULL;
    size_t room = routing_engine_tcp_rx_space(&s_engine, fd, &buf);

    if (room == 0) {
        return;
    }
    ssize_t n = recv(fd, buf, room, 0);
    if (n > 0) {
        routing_engine_tcp_rx(&s_engine, fd, (int)n, now);
    } else if (n == 0) {
        routing_engine_tcp_rx(&s_engine, fd, 0, now);
    } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
        routing_engine_tcp_rx(&s_engine, fd, -1, now);
    }
}

/* Link-local in the backbone zone, or on-link on the backbone; never one of the board's own addresses. */
static bool ripng_source_ok(const backbone_t *bb, const struct sockaddr_in6 *src)
{
    const uint8_t *a = (const uint8_t *)&src->sin6_addr;

    if (is_backbone_address(bb, a)) {
        return false;
    }
    if (is_link_local(a)) {
        return src->sin6_scope_id == 0 || (int)src->sin6_scope_id == bb->index;
    }
    return is_backbone_onlink(bb, a);
}

static void receive_ripng(uint32_t now)
{
    backbone_t bb;

    if (!get_backbone(&bb)) {
        return;
    }
    for (int i = 0; i < RIPNG_RX_PER_WAKE; i++) {
        struct sockaddr_in6 src;
        socklen_t len = sizeof(src);
        ssize_t n = recvfrom(s_ripng_fd, s_ripng_rx, sizeof(s_ripng_rx), 0, (struct sockaddr *)&src, &len);
        if (n < 0) {
            return;
        }
        if (ripng_source_ok(&bb, &src)) {
            routing_engine_ripng_rx(&s_engine, (const uint8_t *)&src.sin6_addr, ntohs(src.sin6_port), s_ripng_rx,
                                    (size_t)n, now);
        }
    }
}

static void add_fd(int fd, fd_set *set, int *max_fd)
{
    FD_SET(fd, set);
    *max_fd = MAX(*max_fd, fd);
}

/* Waits up to `timeout_ms` for a socket and handles what became ready. */
static void wait_sockets(uint32_t timeout_ms)
{
    fd_set rd;
    fd_set wr;
    int max_fd = -1;
    conn_slot_t snapshot[MAX_CONNS];
    struct timeval tv = {.tv_sec = 0, .tv_usec = (suseconds_t)timeout_ms * 1000};

    conn_linger_expire(now_ms());
    FD_ZERO(&rd);
    FD_ZERO(&wr);
    if (s_listen_fd >= 0) {
        add_fd(s_listen_fd, &rd, &max_fd);
    }
    if (s_ripng_fd >= 0) {
        add_fd(s_ripng_fd, &rd, &max_fd);
    }
    memcpy(snapshot, s_conns, sizeof(snapshot));
    for (int i = 0; i < MAX_CONNS; i++) {
        if (snapshot[i].fd >= 0) {
            add_fd(snapshot[i].fd, snapshot[i].connecting ? &wr : &rd, &max_fd);
        }
    }
    if (max_fd < 0) {
        vTaskDelay(pdMS_TO_TICKS(timeout_ms) + 1);
        return;
    }
    if (select(max_fd + 1, &rd, &wr, NULL, &tv) <= 0) {
        return;
    }
    uint32_t now = now_ms();
    if (s_listen_fd >= 0 && FD_ISSET(s_listen_fd, &rd)) {
        accept_one(now);
    }
    if (s_ripng_fd >= 0 && FD_ISSET(s_ripng_fd, &rd)) {
        receive_ripng(now);
    }
    for (int i = 0; i < MAX_CONNS; i++) {
        int fd = snapshot[i].fd;
        conn_slot_t *slot = fd >= 0 ? conn_find(fd) : NULL;
        /* the engine may have closed it (or a new one took the number) meanwhile */
        if (!slot || slot->connecting != snapshot[i].connecting || slot->lingering != snapshot[i].lingering) {
            continue;
        }
        if (slot->lingering) {
            if (FD_ISSET(fd, &rd) && conn_drain(fd)) {
                conn_release(slot);
            }
        } else if (slot->connecting && FD_ISSET(fd, &wr)) {
            finish_connect(slot, now_ms());
        } else if (!slot->connecting && FD_ISSET(fd, &rd)) {
            receive_tcp(fd, now_ms());
        }
    }
}

/* --- status --- */

static void log_changes(const routing_status_t *st)
{
    static const char *const names[] = {"idle", "connect", "active", "opensent", "openconfirm", "established"};
    const routing_settings_t *s = routing_engine_settings(&s_engine);

    for (int i = 0; i < s->peer_count && i < ROUTING_MAX_PEERS; i++) {
        routing_bgp_state_t state = st->peers[i].state;
        if (s->method == ROUTING_METHOD_BGP && state != s_logged_state[i] &&
                (state == ROUTING_BGP_ESTABLISHED || s_logged_state[i] == ROUTING_BGP_ESTABLISHED)) {
            ESP_LOGI(TAG, "BGP peer %d: %s -> %s%s%s", i, names[s_logged_state[i]], names[state],
                     st->peers[i].last_error[0] ? ", last error: " : "", st->peers[i].last_error);
        }
        s_logged_state[i] = state;
    }
}

static void publish_status(uint32_t now)
{
    routing_status_t st;
    routing_metrics_view_t view;

    routing_engine_status(&s_engine, now, &st);
    routing_engine_metrics_view(&s_engine, now, &view);
    log_changes(&st);
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_status = st;
    s_view = view;
    xSemaphoreGive(s_mutex);
}

/* --- task --- */

static bool reached(uint32_t now, uint32_t at)
{
    return (int32_t)(now - at) >= 0;
}

static void routing_task(void *arg)
{
    uint32_t next_state = now_ms();

    routing_engine_start(&s_engine, now_ms());
    for (;;) {
        routing_settings_t s;
        if (xQueueReceive(s_mailbox, &s, 0) == pdTRUE) {
            routing_engine_set_settings(&s_engine, &s, now_ms());
        }
        uint32_t now = now_ms();
        if (reached(now, next_state)) {
            /* a busy OpenThread lock skips the read: retried after 100 ms, so withdrawal stays within 1 s */
            next_state = now + (poll_state(now) ? STATE_PERIOD_MS : STATE_RETRY_MS);
        }
        now = now_ms();
        routing_engine_tick(&s_engine, now);
        publish_status(now);

        uint32_t wait = WAKE_MAX_MS;
        uint32_t deadline = routing_engine_next_deadline_ms(&s_engine);
        if (deadline != UINT32_MAX) {
            wait = reached(now, deadline) ? 0 : MIN(wait, deadline - now);
        }
        wait = reached(now, next_state) ? 0 : MIN(wait, next_state - now);
        wait_sockets(wait);
    }
}

esp_err_t routing_start(void)
{
    routing_settings_t s;

    ESP_RETURN_ON_FALSE(!s_started, ESP_ERR_INVALID_STATE, TAG, "already started");
    esp_err_t err = routing_settings_load(&s);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "stored settings unavailable (%s): using the defaults", esp_err_to_name(err));
    }
    for (int i = 0; i < MAX_CONNS; i++) {
        s_conns[i].fd = -1;
    }
    s_mutex = xSemaphoreCreateMutex();
    s_mailbox = xQueueCreate(1, sizeof(routing_settings_t));
    if (!s_mutex || !s_mailbox) {
        goto no_mem;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_settings = s;
    xSemaphoreGive(s_mutex);
    routing_engine_init(&s_engine, &s_io, &s);
    publish_status(now_ms());
    if (xTaskCreatePinnedToCore(routing_task, "routing", ROUTING_TASK_STACK, NULL, ROUTING_TASK_PRIORITY, NULL,
                                ROUTING_TASK_CORE) != pdPASS) {
        goto no_mem;
    }
    s_started = true;
    ESP_LOGI(TAG, "started, method %d", s.method);
    return ESP_OK;

no_mem:
    if (s_mailbox) {
        vQueueDelete(s_mailbox);
        s_mailbox = NULL;
    }
    if (s_mutex) {
        vSemaphoreDelete(s_mutex);
        s_mutex = NULL;
    }
    ESP_LOGE(TAG, "cannot start the routing task");
    return ESP_ERR_NO_MEM;
}

void routing_on_border_router_up(otInstance *instance)
{
    routing_settings_t s;

    (void)instance; /* the ops use esp_openthread_get_instance() */
    /* the publisher learns the ULA before the first apply sets PD or a custom OMR config (publisher.h) */
    routing_publisher_init(&s_publisher, &routing_ot_ops_openthread);
    if (!s_mutex) {
        if (routing_settings_load(&s) != ESP_OK) {
            ESP_LOGW(TAG, "border router up before routing_start, stored settings unavailable: using the defaults");
        }
        routing_publisher_apply(&s_publisher, &s);
        s_publisher_ready = true;
        return;
    }
    /*
     * Until s_publisher_ready is set the task leaves applying to this hook, so a PUT stored meanwhile (s_settings_gen
     * moved) is applied here as well before the hook hands over.
     */
    for (;;) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s = s_settings;
        uint32_t gen = s_settings_gen;
        xSemaphoreGive(s_mutex);
        routing_publisher_apply(&s_publisher, &s);
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        bool current = gen == s_settings_gen;
        if (current) {
            s_publisher_ready = true;
        }
        xSemaphoreGive(s_mutex);
        if (current) {
            return;
        }
    }
}

esp_err_t routing_apply(const routing_settings_t *s)
{
    ESP_RETURN_ON_FALSE(s_started, ESP_ERR_INVALID_STATE, TAG, "not started");
    ESP_RETURN_ON_ERROR(routing_settings_store(s), TAG, "storing the settings failed");
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_settings = *s;
    s_settings_gen++;
    xSemaphoreGive(s_mutex);
    xQueueOverwrite(s_mailbox, s);
    return ESP_OK;
}

esp_err_t routing_get(routing_settings_t *settings, routing_status_t *status)
{
    ESP_RETURN_ON_FALSE(s_started, ESP_ERR_INVALID_STATE, TAG, "not started");
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (settings) {
        *settings = s_settings;
    }
    if (status) {
        *status = s_status;
    }
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t routing_metrics_view(routing_metrics_view_t *out)
{
    ESP_RETURN_ON_FALSE(s_started, ESP_ERR_INVALID_STATE, TAG, "not started");
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out = s_view;
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

static void add_own(routing_validate_ctx_t *ctx, esp_netif_t *netif)
{
    uint8_t addrs[CONFIG_LWIP_IPV6_NUM_ADDRESSES][16];
    int n = netif_ip6(netif, addrs, CONFIG_LWIP_IPV6_NUM_ADDRESSES);

    for (int i = 0; i < n && ctx->own_count < ROUTING_MAX_OWN_ADDRS; i++) {
        memcpy(ctx->own_addrs[ctx->own_count++], addrs[i], 16);
    }
}

static void add_onlink(routing_validate_ctx_t *ctx, const uint8_t prefix[8])
{
    for (int i = 0; i < ctx->onlink_count; i++) {
        if (memcmp(ctx->onlink[i].b, prefix, 8) == 0) {
            return;
        }
    }
    if (ctx->onlink_count < ROUTING_MAX_ONLINK) {
        memcpy(ctx->onlink[ctx->onlink_count++].b, prefix, 8);
    }
}

esp_err_t routing_validate_ctx_fill(routing_validate_ctx_t *ctx)
{
    esp_netif_t *backbone = netcfg_backbone_netif();
    esp_netif_t *mgmt = netcfg_mgmt_netif();
    uint8_t addrs[CONFIG_LWIP_IPV6_NUM_ADDRESSES][16];

    memset(ctx, 0, sizeof(*ctx));
    add_own(ctx, backbone);
    if (mgmt != backbone) {
        add_own(ctx, mgmt);
    }
    int n = netif_ip6(backbone, addrs, CONFIG_LWIP_IPV6_NUM_ADDRESSES);
    for (int i = 0; i < n; i++) {
        if (!is_link_local(addrs[i])) {
            add_onlink(ctx, addrs[i]);
        }
    }
    if (br_core_get_mode() != BR_MODE_BR_RUNNING) {
        return ESP_OK; /* no border routing manager, no received RAs */
    }
    if (!esp_openthread_lock_acquire(pdMS_TO_TICKS(OT_LOCK_APPLY_MS))) {
        return ESP_ERR_TIMEOUT;
    }
    otInstance *ins = esp_openthread_get_instance();
    if (ins && br_core_get_mode() == BR_MODE_BR_RUNNING) {
        otBorderRoutingPrefixTableIterator it;
        otBorderRoutingPrefixTableEntry entry;
        otBorderRoutingPrefixTableInitIterator(ins, &it);
        while (otBorderRoutingGetNextPrefixTableEntry(ins, &it, &entry) == OT_ERROR_NONE) {
            if (entry.mIsOnLink && entry.mPrefix.mLength == 64) {
                add_onlink(ctx, entry.mPrefix.mPrefix.mFields.m8);
            }
        }
    }
    esp_openthread_lock_release();
    return ESP_OK;
}
