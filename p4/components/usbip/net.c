// USB/IP network side (spec sections 5, 6.1, 7, 8.2): the TCP USBIP_PORT listener, the connections that send one OP
// request, the one session, and the network task that owns every socket and is the only caller of the engine and of
// the USB side (open, close, strings, the ops).
//
// The network task waits in select() over the listener, the session's socket, the pending connections and an eventfd
// the USB task writes after each message it queues (usbip_usb_set_wake); the queue is drained between selects. Every
// wait is at most WAIT_MAX_MS, the period of the engine's tick (the EP0 unlink deadline) and of the deadlines here.
//
// Connections (spec 6.1): accepted on any address, then held to netcfg's NETCFG_SVC_USBIP rules (the local address
// and the route back to the peer: the mesh refusal and, in split mode, mgmt only). A connection must send 0x01 first
// within USBIP_FIRST_BYTE_MS (the whole OP request within that time); anything else is closed without a reply and
// counted as refused. OP_REQ_DEVLIST is answered at any time and the connection closed; OP_REQ_IMPORT follows the
// table of 6.1; another code or version closes it.
//
// Session end, for any cause (the client's close, keepalive, a protocol error, a send failure, a client that does not
// read, a kick, the engine's end_session, the device gone): the socket is closed first, then usbip_engine_stop, which
// power-cycles the root port (unless the device is gone). The device then goes away (USBIP_EV_DEV_GONE: closed here)
// and comes back (USBIP_EV_DEV_NEW: opened, exportable again); until it is gone it is not offered (RESET_WAIT_MS).
//
// Replies: the engine's emit writes to the socket without blocking; what the socket does not take waits in a PSRAM
// ring (TX_RING_LEN) and goes out as the socket drains. A ring that would overflow, or one that makes no progress for
// TX_STALL_MS, ends the session: the client does not read its replies. The socket is always read (an UNLINK is never
// stuck behind replies).
#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "board_cores.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_eventfd.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "netcfg.h"
#include "usbip.h" // USBIP_SOCKETS
#include "usbip_priv.h"

static const char *TAG = "usbip";

// Management core, beside the RCP proxy's sockets: the OpenThread task and the radio transports stay on NET_CORE.
#define TASK_STACK 6144
#define TASK_PRIORITY 6
#define LISTEN_BACKLOG 2
// Connections that have not sent their OP request yet; a new one takes the oldest one's place when all are used.
#define PENDING_MAX 2
_Static_assert(1 + 1 + PENDING_MAX + 1 <= USBIP_SOCKETS, "USBIP_SOCKETS (usbip.h) counts the sockets net.c holds");
#define OP_REQ_MAX (USBIP_OP_HDR_LEN + USBIP_BUSID_LEN) // OP_REQ_IMPORT: the header and the bus id
#define DEVLIST_REPLY_MAX (12 + USBIP_DEV_REC_LEN + USBIP_IFACES_MAX * USBIP_IFACE_REC_LEN)
#define IMPORT_REPLY_MAX (8 + USBIP_DEV_REC_LEN)
#define WAIT_MAX_MS 100
#define LISTEN_RETRY_MS 5000
#define ACCEPT_BACKOFF_MS 1000 // accept() failing (no free socket): the listener rests that long
#define CLOSE_RETRY_MS 500     // a device close esp-usb refused
#define RESET_WAIT_MS 5000     // after a session: the device's power-cycle shows as DEV_GONE within this
#define TX_RING_LEN (256 * 1024)
#define TX_STALL_MS 30000
#define RX_ROUND_MAX (64 * 1024) // session bytes read per wake-up before the events get a turn
#define SKIP_CHUNK 256
#define ISO_DESC_LEN 16
#define ISO_PACKETS_MAX 1024 // Linux USBIP_MAX_ISO_PACKETS
#define REFUSE_LOG_MS 10000
#define SELECT_ERR_LOG_MS 10000
#define SELECT_ERR_DELAY_MS 100
#define WHY_LEN 96

typedef struct {
    int fd; // -1: free
    int64_t deadline_ms;
    size_t len;
    uint8_t buf[OP_REQ_MAX];
    struct sockaddr_storage peer;
} pending_t;

typedef enum {
    RX_HDR,  // the 48-byte command header
    RX_DATA, // a CMD_SUBMIT's OUT data (at most USBIP_XFER_MAX)
    RX_SKIP, // bytes read and dropped: OUT data over USBIP_XFER_MAX, isochronous packet descriptors
} rx_phase_t;

typedef struct {
    int fd; // -1 without a session
    rx_phase_t phase;
    uint8_t hdr[USBIP_CMD_HDR_LEN];
    size_t have; // bytes of the current header or data
    usbip_cmd_t cmd;
    size_t data_len; // OUT data kept for the engine
    size_t skip;     // bytes still to drop before the command goes to the engine
    bool data_dropped;
} session_t;

/* ---- state (network task only, unless noted) ---- */

#define STR_LEN sizeof(((usbip_status_t *)0)->manufacturer) // also the product's and the serial's

// What usbip_net_start allocates (PSRAM): with USB/IP off none of it takes internal RAM.
typedef struct {
    usbip_engine_t engine;
    usbip_engine_ops_t ops;
    pending_t pending[PENDING_MAX];
    session_t sess;
    uint8_t skip_buf[SKIP_CHUNK];
    uint8_t reply[DEVLIST_REPLY_MAX];
    // The device: opened (exportable or attached), unsupported, or none (see the flags below).
    usbip_dev_rec_t dev;
    usbip_dev_rec_t rec; // on_dev_new's open
    char reason[WHY_LEN];
    char end_why[WHY_LEN];
    char manufacturer[STR_LEN], product[STR_LEN], serial[STR_LEN]; // publish's read of the strings
    // What usbip_net_snapshot reads: written by the network task under s_lock.
    usbip_status_t pub;
} net_t;

static net_t *s_net; // set by usbip_net_start, never freed
static bool s_started; // usbip_net_start's caller
static QueueHandle_t s_events;
static int s_efd = -1;
static atomic_bool s_kick;

static int s_lfd = -1;
static int64_t s_listen_retry_ms;
static int64_t s_accept_rest_ms;
static uint8_t *s_rx_data; // USBIP_XFER_MAX, PSRAM

static uint8_t *s_tx; // TX_RING_LEN, PSRAM
static size_t s_tx_head, s_tx_len;
static int64_t s_tx_progress_ms; // the last byte sent, or the ring's first byte queued

static bool s_has_dev;
static const uint8_t *s_cfg;
static size_t s_cfg_len;
static bool s_unsupported;
static int64_t s_reset_until_ms; // > 0: a session ended, the device's power-cycle is awaited (not offered)
static bool s_close_pending;
static int64_t s_close_retry_ms;

static uint32_t s_sessions, s_refused;
static int64_t s_refuse_log_ms = -REFUSE_LOG_MS;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED; // s_net->pub
static bool s_dirty; // the device or client part changed: copy it at the next publish

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static void note_error(const char *why)
{
    if (s_net == NULL) {
        return;
    }
    taskENTER_CRITICAL(&s_lock);
    strlcpy(s_net->pub.last_error, why, sizeof(s_net->pub.last_error));
    taskEXIT_CRITICAL(&s_lock);
}

static void set_nonblocking(int fd)
{
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
}

static bool would_block(void)
{
    return errno == EAGAIN || errno == EWOULDBLOCK;
}

// "192.168.10.20:51322" or "[2001:db8::1]:51322"; cut to cap (a long IPv6 address does not fit the status' 48).
static void peer_str(const struct sockaddr_storage *sa, char *out, size_t cap)
{
    char ip[INET6_ADDRSTRLEN] = "?";
    char s[INET6_ADDRSTRLEN + 16];

    if (sa->ss_family == AF_INET) {
        const struct sockaddr_in *v4 = (const struct sockaddr_in *)sa;

        inet_ntop(AF_INET, &v4->sin_addr, ip, sizeof(ip));
        snprintf(s, sizeof(s), "%s:%u", ip, ntohs(v4->sin_port));
    } else {
        const struct sockaddr_in6 *v6 = (const struct sockaddr_in6 *)sa;
        const uint8_t *a = v6->sin6_addr.s6_addr;
        static const uint8_t mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};

        if (memcmp(a, mapped, sizeof(mapped)) == 0) { // an IPv4 client on the dual-stack listener
            inet_ntop(AF_INET, a + 12, ip, sizeof(ip));
            snprintf(s, sizeof(s), "%s:%u", ip, ntohs(v6->sin6_port));
        } else {
            inet_ntop(AF_INET6, a, ip, sizeof(ip));
            snprintf(s, sizeof(s), "[%s]:%u", ip, ntohs(v6->sin6_port));
        }
    }
    strlcpy(out, s, cap);
}

static usbip_state_t state_now(void)
{
    if (s_net->sess.fd >= 0) {
        return USBIP_STATE_ATTACHED;
    }
    if (s_has_dev && s_reset_until_ms == 0) {
        return USBIP_STATE_EXPORTABLE;
    }
    return s_unsupported ? USBIP_STATE_UNSUPPORTED : USBIP_STATE_NO_DEVICE;
}

// Non-control endpoints over the interfaces' alternate setting 0 (the status' "endpoints").
static uint8_t count_endpoints(const uint8_t *cfg, size_t len)
{
    unsigned n = 0;

    for (size_t off = 0; off + 2 <= len && cfg[off] >= 2;) {
        const uint8_t blen = cfg[off];

        if (cfg[off + 1] == 4 /* INTERFACE */ && blen >= 9 && off + 9 <= len && cfg[off + 3] == 0) {
            n += cfg[off + 4];
        }
        off += blen;
    }
    return n > UINT8_MAX ? UINT8_MAX : (uint8_t)n;
}

/* ---- the published status ---- */

static void publish(void)
{
    usbip_counters_t c;
    usbip_mem_stats_t m;

    usbip_engine_counters(&s_net->engine, &c);
    usbip_engine_mem(&s_net->engine, &m);
    taskENTER_CRITICAL(&s_lock);
    s_net->pub.state = state_now();
    s_net->pub.counters = c;
    s_net->pub.mem = m;
    s_net->pub.sessions = s_sessions;
    s_net->pub.refused = s_refused;
    taskEXIT_CRITICAL(&s_lock);
    if (!s_dirty) {
        return;
    }
    s_dirty = false;
    // The strings come from esp-usb's device info: read here, on the network task, not under the lock.
    const bool dev = s_has_dev && s_reset_until_ms == 0;

    if (dev) {
        usbip_usb_strings(s_net->manufacturer, s_net->product, s_net->serial, sizeof(s_net->manufacturer));
    }
    const uint8_t endpoints = dev ? count_endpoints(s_cfg, s_cfg_len) : 0;

    taskENTER_CRITICAL(&s_lock);
    s_net->pub.has_device = dev;
    if (dev) {
        s_net->pub.dev = s_net->dev;
        memcpy(s_net->pub.manufacturer, s_net->manufacturer, sizeof(s_net->pub.manufacturer));
        memcpy(s_net->pub.product, s_net->product, sizeof(s_net->pub.product));
        memcpy(s_net->pub.serial, s_net->serial, sizeof(s_net->pub.serial));
    }
    s_net->pub.endpoints = endpoints;
    strlcpy(s_net->pub.reason, s_unsupported && !s_has_dev ? s_net->reason : "", sizeof(s_net->pub.reason));
    s_net->pub.has_client = s_net->sess.fd >= 0;
    taskEXIT_CRITICAL(&s_lock);
}

void usbip_net_snapshot(usbip_status_t *out)
{
    if (out == NULL) {
        return;
    }
    if (s_net == NULL) {
        out->state = USBIP_STATE_NO_DEVICE;
        return;
    }
    taskENTER_CRITICAL(&s_lock);
    out->state = s_net->pub.state;
    memcpy(out->reason, s_net->pub.reason, sizeof(out->reason));
    out->has_device = s_net->pub.has_device;
    out->dev = s_net->pub.dev;
    memcpy(out->manufacturer, s_net->pub.manufacturer, sizeof(out->manufacturer));
    memcpy(out->product, s_net->pub.product, sizeof(out->product));
    memcpy(out->serial, s_net->pub.serial, sizeof(out->serial));
    out->endpoints = s_net->pub.endpoints;
    out->has_client = s_net->pub.has_client;
    memcpy(out->client, s_net->pub.client, sizeof(out->client));
    out->client_since = s_net->pub.client_since;
    out->counters = s_net->pub.counters;
    out->sessions = s_net->pub.sessions;
    out->refused = s_net->pub.refused;
    out->mem = s_net->pub.mem;
    memcpy(out->last_error, s_net->pub.last_error, sizeof(out->last_error));
    taskEXIT_CRITICAL(&s_lock);
}

/* ---- session end ---- */

// Closes the session's connection. Safe inside the engine's callbacks (it does not call the engine): the engine is
// stopped by after_engine, or by the engine itself after its end_session. error: why goes to last_error.
static void session_close(const char *why, bool error)
{
    if (s_net->sess.fd < 0) {
        return;
    }
    close(s_net->sess.fd);
    s_net->sess.fd = -1;
    s_tx_head = 0;
    s_tx_len = 0;
    strlcpy(s_net->end_why, why, sizeof(s_net->end_why));
    // The engine's stop power-cycles the root port: the device is offered again once it has gone and come back.
    s_reset_until_ms = now_ms() + RESET_WAIT_MS;
    s_dirty = true;
    // The engine's stop logs "session ended" with the same reason.
    if (error) {
        ESP_LOGW(TAG, "connection of %s closed: %s", s_net->pub.client, why);
        note_error(why);
    } else {
        ESP_LOGI(TAG, "connection of %s closed: %s", s_net->pub.client, why);
    }
}

// After any engine call (and any session_close): a session whose connection is gone is stopped.
static void after_engine(void)
{
    if (s_net->sess.fd < 0 && usbip_engine_active(&s_net->engine)) {
        usbip_engine_stop(&s_net->engine, s_net->end_why);
    }
}

static void session_end(const char *why, bool error)
{
    session_close(why, error);
    after_engine();
}

/* ---- replies ---- */

static void tx_push(const uint8_t *d, size_t n)
{
    while (n > 0) {
        const size_t tail = (s_tx_head + s_tx_len) % TX_RING_LEN;
        const size_t run = TX_RING_LEN - tail < n ? TX_RING_LEN - tail : n;

        memcpy(s_tx + tail, d, run);
        s_tx_len += run;
        d += run;
        n -= run;
    }
}

// Sends what the ring holds without blocking; what the socket does not take stays at the ring's head.
static void tx_flush(void)
{
    while (s_net->sess.fd >= 0 && s_tx_len > 0) {
        const size_t first = TX_RING_LEN - s_tx_head < s_tx_len ? TX_RING_LEN - s_tx_head : s_tx_len;
        struct iovec iov[2] = {
            {.iov_base = s_tx + s_tx_head, .iov_len = first},
            {.iov_base = s_tx, .iov_len = s_tx_len - first},
        };
        struct msghdr m = {.msg_iov = iov, .msg_iovlen = s_tx_len > first ? 2 : 1};
        const ssize_t r = sendmsg(s_net->sess.fd, &m, MSG_DONTWAIT);

        if (r < 0) {
            if (!would_block()) {
                char why[WHY_LEN];

                snprintf(why, sizeof(why), "sending to the client failed (errno %d)", errno);
                session_close(why, true);
            }
            return;
        }
        if (r == 0) {
            return;
        }
        s_tx_head = (s_tx_head + (size_t)r) % TX_RING_LEN;
        s_tx_len -= (size_t)r;
        s_tx_progress_ms = now_ms();
    }
}

// Writes a then b to the session: straight to the socket while nothing waits, the rest into the ring. A ring that
// would overflow closes the session. Never blocks; never calls the engine.
static void tx_write(const uint8_t *a, size_t an, const uint8_t *b, size_t bn)
{
    size_t sent = 0;

    if (s_net->sess.fd < 0) {
        return;
    }
    if (s_tx_len == 0) {
        struct iovec iov[2] = {{.iov_base = (void *)a, .iov_len = an}, {.iov_base = (void *)b, .iov_len = bn}};
        struct msghdr m = {.msg_iov = iov, .msg_iovlen = bn > 0 ? 2 : 1};
        const ssize_t r = sendmsg(s_net->sess.fd, &m, MSG_DONTWAIT);

        if (r < 0 && !would_block()) {
            char why[WHY_LEN];

            snprintf(why, sizeof(why), "sending to the client failed (errno %d)", errno);
            session_close(why, true);
            return;
        }
        sent = r > 0 ? (size_t)r : 0;
        if (sent == an + bn) {
            return;
        }
        s_tx_progress_ms = now_ms(); // the ring starts now
    }
    if (an + bn - sent > TX_RING_LEN - s_tx_len) {
        session_close("the client does not read its replies (reply buffer full)", true);
        return;
    }
    if (sent < an) {
        tx_push(a + sent, an - sent);
        tx_push(b, bn);
    } else {
        tx_push(b + (sent - an), bn - (sent - an));
    }
}

/* ---- the engine's ops: the USB side's, plus these ---- */

static void op_emit(void *ctx, const uint8_t hdr[USBIP_ENGINE_HDR_LEN], const uint8_t *data, size_t n)
{
    (void)ctx;
    tx_write(hdr, USBIP_ENGINE_HDR_LEN, data, n);
}

static void op_end_session(void *ctx, const char *why)
{
    (void)ctx;
    session_close(why, true); // the engine stops itself right after
}

static int64_t op_now_ms(void *ctx)
{
    (void)ctx;
    return now_ms();
}

/* ---- the session's commands ---- */

static void rx_reset(void)
{
    s_net->sess.phase = RX_HDR;
    s_net->sess.have = 0;
    s_net->sess.data_len = 0;
    s_net->sess.skip = 0;
    s_net->sess.data_dropped = false;
}

static void dispatch(void)
{
    const uint8_t *out = s_net->sess.data_len > 0 && !s_net->sess.data_dropped ? s_rx_data : NULL;

    usbip_engine_on_cmd(&s_net->engine, &s_net->sess.cmd, out);
    after_engine();
    rx_reset();
}

// The header is complete: what follows it in the stream (OUT data, isochronous packet descriptors) is read next. False
// on a protocol error.
static bool rx_header_done(void)
{
    if (usbip_proto_parse_cmd(s_net->sess.hdr, &s_net->sess.cmd) != ESP_OK) {
        return false;
    }
    if (s_net->sess.cmd.command != USBIP_CMD_SUBMIT) {
        dispatch();
        return true;
    }
    const int32_t len = s_net->sess.cmd.submit.length;
    const int32_t packets = s_net->sess.cmd.submit.num_packets;
    const bool out = s_net->sess.cmd.direction == USBIP_DIR_OUT;

    if ((out && len < 0) || packets > ISO_PACKETS_MAX) {
        return false; // the stream's length is unknown
    }
    const size_t data = out && len > 0 ? (size_t)len : 0;
    const size_t iso = packets > 0 ? (size_t)packets * ISO_DESC_LEN : 0; // -1 (0xffffffff): not isochronous

    s_net->sess.have = 0;
    if (data > USBIP_XFER_MAX) { // dropped: the engine answers -EINVAL from the length alone
        s_net->sess.data_dropped = true;
        s_net->sess.data_len = data;
        s_net->sess.skip = data + iso;
        s_net->sess.phase = RX_SKIP;
    } else if (data > 0) {
        s_net->sess.data_len = data;
        s_net->sess.skip = iso;
        s_net->sess.phase = RX_DATA;
    } else if (iso > 0) {
        s_net->sess.skip = iso;
        s_net->sess.phase = RX_SKIP;
    } else {
        dispatch();
    }
    return true;
}

// n bytes arrived into the current phase.
static bool rx_advance(size_t n)
{
    switch (s_net->sess.phase) {
    case RX_HDR:
        s_net->sess.have += n;
        return s_net->sess.have < USBIP_CMD_HDR_LEN ? true : rx_header_done();
    case RX_DATA:
        s_net->sess.have += n;
        if (s_net->sess.have == s_net->sess.data_len) {
            if (s_net->sess.skip > 0) {
                s_net->sess.phase = RX_SKIP;
            } else {
                dispatch();
            }
        }
        return true;
    case RX_SKIP:
        s_net->sess.skip -= n;
        if (s_net->sess.skip == 0) {
            dispatch();
        }
        return true;
    }
    return false;
}

static void read_session(void)
{
    size_t round = 0;

    while (s_net->sess.fd >= 0 && round < RX_ROUND_MAX) {
        uint8_t *dst;
        size_t room;

        switch (s_net->sess.phase) {
        case RX_HDR:
            dst = s_net->sess.hdr + s_net->sess.have;
            room = USBIP_CMD_HDR_LEN - s_net->sess.have;
            break;
        case RX_DATA:
            dst = s_rx_data + s_net->sess.have;
            room = s_net->sess.data_len - s_net->sess.have;
            break;
        default:
            dst = s_net->skip_buf;
            room = s_net->sess.skip < SKIP_CHUNK ? s_net->sess.skip : SKIP_CHUNK;
            break;
        }
        const ssize_t n = recv(s_net->sess.fd, dst, room, MSG_DONTWAIT);

        if (n == 0) {
            session_end("the client closed the connection", false);
            return;
        }
        if (n < 0) {
            if (would_block()) {
                return;
            }
            char why[WHY_LEN];

            if (errno == ETIMEDOUT || errno == ECONNABORTED) { // lwIP aborts the connection on a keepalive timeout
                snprintf(why, sizeof(why), "the client stopped answering (keepalive)");
            } else if (errno == ECONNRESET) {
                snprintf(why, sizeof(why), "the client reset the connection");
            } else {
                snprintf(why, sizeof(why), "receiving from the client failed (errno %d)", errno);
            }
            session_end(why, true);
            return;
        }
        round += (size_t)n;
        if (!rx_advance((size_t)n)) {
            session_end("protocol error: not a CMD_SUBMIT or CMD_UNLINK header", true);
            return;
        }
    }
}

/* ---- connections ---- */

static void refuse_log(const char *peer, const char *why)
{
    const int64_t now = now_ms();

    if (now - s_refuse_log_ms >= REFUSE_LOG_MS) {
        s_refuse_log_ms = now;
        ESP_LOGW(TAG, "connection from %s refused: %s (%lu refused so far)", peer, why, (unsigned long)s_refused);
    }
}

static void pending_close(pending_t *p, bool refused, const char *why)
{
    char peer[48];

    peer_str(&p->peer, peer, sizeof(peer));
    close(p->fd);
    p->fd = -1;
    if (refused) {
        s_refused++;
        refuse_log(peer, why);
    } else {
        ESP_LOGI(TAG, "connection from %s closed: %s", peer, why);
    }
}

// A short reply on a fresh connection: it fits the empty send buffer; a partial send is not retried (closed anyway).
static void reply_and_close(pending_t *p, size_t n, const char *what)
{
    if (n == 0 || send(p->fd, s_net->reply, n, MSG_DONTWAIT) != (ssize_t)n) {
        ESP_LOGW(TAG, "%s: the reply could not be sent", what);
    }
    pending_close(p, false, what);
}

static void session_setup(int fd)
{
    const int one = 1;
    const int idle = USBIP_KEEPALIVE_IDLE_S;
    const int intvl = USBIP_KEEPALIVE_INTERVAL_S;
    const int count = USBIP_KEEPALIVE_PROBES;

    // Each URB is a request and reply: Nagle plus delayed ACK would add ~200 ms stalls.
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    // A vanished client frees the device within about 25 s instead of lwIP's two hours.
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count));
}

// Spec 6.1's IMPORT table: another bus id or no exportable device -> ST_NODEV, a session already -> ST_DEV_BUSY,
// else status 0 with the record and the connection becomes the session.
static void answer_import(pending_t *p)
{
    char busid[USBIP_BUSID_LEN + 1];

    memcpy(busid, p->buf + USBIP_OP_HDR_LEN, USBIP_BUSID_LEN);
    busid[USBIP_BUSID_LEN] = '\0';
    if (strcmp(busid, USBIP_BUSID) != 0 || (s_net->sess.fd < 0 && state_now() != USBIP_STATE_EXPORTABLE)) {
        reply_and_close(p, usbip_proto_encode_import_reply(USBIP_ST_NODEV, NULL, s_net->reply, sizeof(s_net->reply)),
                        "import refused: no exportable device by that bus id");
        return;
    }
    if (s_net->sess.fd >= 0 || usbip_engine_active(&s_net->engine)) {
        reply_and_close(p, usbip_proto_encode_import_reply(USBIP_ST_DEV_BUSY, NULL, s_net->reply, sizeof(s_net->reply)),
                        "import refused: the device is in use");
        return;
    }
    const esp_err_t err = usbip_engine_start(&s_net->engine, &s_net->ops, NULL, &s_net->dev, s_cfg, s_cfg_len);

    if (err != ESP_OK) {
        char why[WHY_LEN];

        snprintf(why, sizeof(why), "import failed: the interfaces could not be claimed (%s)", esp_err_to_name(err));
        note_error(why);
        reply_and_close(p, usbip_proto_encode_import_reply(USBIP_ST_NODEV, NULL, s_net->reply, sizeof(s_net->reply)), why);
        return;
    }
    char client[48];

    peer_str(&p->peer, client, sizeof(client));
    session_setup(p->fd);
    s_net->sess.fd = p->fd;
    p->fd = -1;
    rx_reset();
    s_tx_head = 0;
    s_tx_len = 0;
    s_sessions++;
    s_dirty = true;
    taskENTER_CRITICAL(&s_lock);
    strlcpy(s_net->pub.client, client, sizeof(s_net->pub.client));
    // 0 while the clock is unset (before 2020-01-01), as zigbee reports it.
    const time_t now = time(NULL);
    s_net->pub.client_since = now < 1577836800 ? 0 : (int64_t)now;
    taskEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "session %lu: %s imported %s (%04x:%04x)", (unsigned long)s_sessions, client, USBIP_BUSID,
             s_net->dev.vid, s_net->dev.pid);
    const size_t n = usbip_proto_encode_import_reply(USBIP_ST_OK, &s_net->dev, s_net->reply, sizeof(s_net->reply));

    tx_write(s_net->reply, n, NULL, 0);
    after_engine();
}

static void pending_read(pending_t *p)
{
    const size_t need = p->len < USBIP_OP_HDR_LEN ? USBIP_OP_HDR_LEN : OP_REQ_MAX;
    const ssize_t n = recv(p->fd, p->buf + p->len, need - p->len, MSG_DONTWAIT);

    if (n == 0) {
        pending_close(p, p->len == 0, "closed before its request");
        return;
    }
    if (n < 0) {
        if (!would_block()) {
            pending_close(p, false, "receive failed");
        }
        return;
    }
    if (p->len == 0 && !usbip_proto_first_byte_ok(p->buf[0])) {
        pending_close(p, true, "not a USB/IP request (an HTTP request from a web page?)");
        return;
    }
    p->len += (size_t)n;
    if (p->len < USBIP_OP_HDR_LEN) {
        return;
    }
    uint16_t code = 0;
    const esp_err_t err = usbip_proto_parse_op_header(p->buf, &code);

    if (err != ESP_OK) {
        pending_close(p, false, err == ESP_ERR_INVALID_VERSION ? "another USB/IP version" : "an unsupported request");
        return;
    }
    if (code == USBIP_OP_REQ_DEVLIST) {
        const bool listed = s_has_dev && (s_net->sess.fd >= 0 || s_reset_until_ms == 0);

        reply_and_close(p, usbip_proto_encode_devlist(listed ? &s_net->dev : NULL, s_net->reply, sizeof(s_net->reply)),
                        "device list sent");
        return;
    }
    if (p->len == OP_REQ_MAX) {
        answer_import(p);
    }
}

static void accept_one(void)
{
    struct sockaddr_storage sa;
    socklen_t len = sizeof(sa);
    const int fd = accept(s_lfd, (struct sockaddr *)&sa, &len);

    if (fd < 0) {
        if (!would_block()) {
            ESP_LOGW(TAG, "accept: errno %d", errno);
            s_accept_rest_ms = now_ms() + ACCEPT_BACKOFF_MS;
        }
        return;
    }
    // Both access rules: the local address it came in on, and the route back to it (a Thread mesh node that dials an
    // infrastructure address passes the first and is refused by the second). Called here, never from tcpip.
    if (!netcfg_socket_allowed(fd, NETCFG_SVC_USBIP) ||
            !netcfg_peer_allowed_as((const struct sockaddr *)&sa, NETCFG_SVC_USBIP, "a client")) {
        char peer[48];

        peer_str(&sa, peer, sizeof(peer));
        close(fd);
        s_refused++;
        refuse_log(peer, "not allowed on this interface");
        return;
    }
    set_nonblocking(fd);
    pending_t *slot = NULL;

    for (int i = 0; i < PENDING_MAX; i++) {
        if (s_net->pending[i].fd < 0) {
            slot = &s_net->pending[i];
            break;
        }
        if (slot == NULL || s_net->pending[i].deadline_ms < slot->deadline_ms) {
            slot = &s_net->pending[i];
        }
    }
    if (slot->fd >= 0) {
        pending_close(slot, slot->len == 0, "gave way to a newer connection before its request");
    }
    *slot = (pending_t) {
        .fd = fd, .deadline_ms = now_ms() + USBIP_FIRST_BYTE_MS, .len = 0, .peer = sa
    };
}

static void listen_open(void)
{
    const int fd = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
    const int one = 1;
    const int zero = 0;
    char why[WHY_LEN];

    if (fd < 0) {
        snprintf(why, sizeof(why), "listener: no socket (errno %d)", errno);
        goto fail;
    }
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof(zero)); // dual-stack: IPv4 clients too
    struct sockaddr_in6 sa = {
        .sin6_family = AF_INET6,
        .sin6_port = htons(USBIP_PORT),
        .sin6_addr = IN6ADDR_ANY_INIT,
    };

    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0 || listen(fd, LISTEN_BACKLOG) != 0) {
        snprintf(why, sizeof(why), "listener: TCP port %u unavailable (errno %d)", USBIP_PORT, errno);
        close(fd);
        goto fail;
    }
    set_nonblocking(fd);
    s_lfd = fd;
    ESP_LOGI(TAG, "listening on TCP port %u", USBIP_PORT);
    return;
fail:
    ESP_LOGE(TAG, "%s: retrying in %d s", why, LISTEN_RETRY_MS / 1000);
    note_error(why);
    s_listen_retry_ms = now_ms() + LISTEN_RETRY_MS;
}

/* ---- device events ---- */

static void device_close(void)
{
    s_close_pending = usbip_usb_close() != ESP_OK;
    s_close_retry_ms = now_ms() + CLOSE_RETRY_MS;
}

static void on_dev_new(void)
{
    const uint8_t *cfg = NULL;
    size_t cfg_len = 0;
    char why[WHY_LEN];

    if (s_close_pending) {
        device_close();
    }
    const esp_err_t err = usbip_usb_open(&s_net->rec, &cfg, &cfg_len, why, sizeof(why));

    if (err == ESP_ERR_INVALID_STATE) {
        ESP_LOGD(TAG, "new device: %s", why); // reported twice at start, or the old one is not closed yet
        return;
    }
    if (err == ESP_ERR_NOT_FOUND) {
        ESP_LOGD(TAG, "new device: gone again");
        return;
    }
    s_dirty = true;
    if (err != ESP_OK) {
        s_has_dev = false;
        s_unsupported = true;
        strlcpy(s_net->reason, why[0] != '\0' ? why : esp_err_to_name(err), sizeof(s_net->reason));
        ESP_LOGW(TAG, "device not exportable: %s", s_net->reason);
        return;
    }
    s_net->dev = s_net->rec;
    s_cfg = cfg;
    s_cfg_len = cfg_len;
    s_has_dev = true;
    s_unsupported = false;
    s_net->reason[0] = '\0';
    s_reset_until_ms = 0;
    ESP_LOGI(TAG, "device %04x:%04x exportable as %s", s_net->dev.vid, s_net->dev.pid, USBIP_BUSID);
}

static void on_dev_gone(void)
{
    if (s_net->sess.fd >= 0) {
        session_close("the device went away", true);
    }
    usbip_engine_stop(&s_net->engine, "the device went away"); // no power-cycle: esp-usb recovers the port itself
    if (s_has_dev || s_unsupported) {
        ESP_LOGI(TAG, "device gone");
    }
    s_has_dev = false;
    s_cfg = NULL;
    s_cfg_len = 0;
    s_unsupported = false;
    s_net->reason[0] = '\0';
    s_reset_until_ms = 0;
    s_dirty = true;
    device_close();
}

// The queue first, its eventfd already read: a message posted after the read writes the eventfd again.
static void drain_events(void)
{
    usbip_event_t ev;

    for (int i = 0; i < USBIP_USB_EVENTS_LEN && xQueueReceive(s_events, &ev, 0) == pdTRUE; i++) {
        switch (ev.type) {
        case USBIP_EV_DEV_NEW:
            on_dev_new();
            break;
        case USBIP_EV_DEV_GONE:
            on_dev_gone();
            break;
        case USBIP_EV_XFER_DONE:
            usbip_engine_on_complete(&s_net->engine, ev.x, ev.st, ev.actual);
            after_engine();
            break;
        }
    }
}

/* ---- the network task ---- */

static int fd_add(int fd, fd_set *set, int maxfd)
{
    FD_SET(fd, set);
    return fd > maxfd ? fd : maxfd;
}

// Deadlines and the engine's tick, once per wake-up (at most WAIT_MAX_MS apart).
static void timers(void)
{
    const int64_t now = now_ms();

    usbip_engine_tick(&s_net->engine);
    after_engine();
    if (s_net->sess.fd >= 0 && s_tx_len > 0 && now - s_tx_progress_ms >= TX_STALL_MS) {
        session_end("the client has not read its replies for 30 s", true);
    }
    for (int i = 0; i < PENDING_MAX; i++) {
        pending_t *p = &s_net->pending[i];

        if (p->fd >= 0 && now >= p->deadline_ms) {
            pending_close(p, p->len == 0, p->len == 0 ? "sent nothing within 10 s" : "incomplete request in 10 s");
        }
    }
    if (s_close_pending && now >= s_close_retry_ms) {
        device_close();
    }
    if (s_reset_until_ms > 0 && now >= s_reset_until_ms) {
        s_reset_until_ms = 0;
        s_dirty = true;
        if (s_has_dev) {
            ESP_LOGW(TAG, "the device did not reset after the session: offered again as it is");
        }
    }
}

static void net_task(void *arg)
{
    (void)arg;
    int64_t select_err_log_ms = -SELECT_ERR_LOG_MS;

    for (;;) {
        const int64_t now = now_ms();

        if (s_lfd < 0 && now >= s_listen_retry_ms) {
            listen_open();
        }
        fd_set rd, wr;
        int maxfd = s_efd;

        FD_ZERO(&rd);
        FD_ZERO(&wr);
        FD_SET(s_efd, &rd);
        if (s_lfd >= 0 && now >= s_accept_rest_ms) {
            maxfd = fd_add(s_lfd, &rd, maxfd);
        }
        if (s_net->sess.fd >= 0) {
            maxfd = fd_add(s_net->sess.fd, &rd, maxfd);
            if (s_tx_len > 0) {
                maxfd = fd_add(s_net->sess.fd, &wr, maxfd);
            }
        }
        for (int i = 0; i < PENDING_MAX; i++) {
            if (s_net->pending[i].fd >= 0) {
                maxfd = fd_add(s_net->pending[i].fd, &rd, maxfd);
            }
        }
        const int wait_ms = uxQueueMessagesWaiting(s_events) > 0 ? 0 : WAIT_MAX_MS;
        struct timeval tv = {.tv_sec = 0, .tv_usec = wait_ms * 1000};
        const int ready = select(maxfd + 1, &rd, &wr, NULL, &tv);

        if (ready < 0) {
            const int64_t t = now_ms();

            if (t - select_err_log_ms >= SELECT_ERR_LOG_MS) {
                ESP_LOGE(TAG, "select: errno %d", errno);
                select_err_log_ms = t;
            }
            vTaskDelay(pdMS_TO_TICKS(SELECT_ERR_DELAY_MS));
            FD_ZERO(&rd);
            FD_ZERO(&wr);
        } else if (ready == 0) {
            FD_ZERO(&rd);
            FD_ZERO(&wr);
        }
        if (FD_ISSET(s_efd, &rd)) {
            uint64_t v;

            if (read(s_efd, &v, sizeof(v)) != sizeof(v)) {
                ESP_LOGW(TAG, "eventfd read failed");
            }
        }
        drain_events();
        if (atomic_exchange(&s_kick, false) && s_net->sess.fd >= 0) {
            session_end("kicked", false);
        }
        // The session before the listener: a client that closes and imports again at once is seen closing first.
        const int sfd = s_net->sess.fd;

        if (sfd >= 0 && FD_ISSET(sfd, &wr)) {
            tx_flush();
            after_engine();
        }
        if (sfd >= 0 && s_net->sess.fd == sfd && FD_ISSET(sfd, &rd)) {
            read_session();
        }
        for (int i = 0; i < PENDING_MAX; i++) {
            if (s_net->pending[i].fd >= 0 && FD_ISSET(s_net->pending[i].fd, &rd)) {
                pending_read(&s_net->pending[i]);
            }
        }
        if (s_lfd >= 0 && FD_ISSET(s_lfd, &rd)) {
            accept_one();
        }
        timers();
        publish();
    }
}

static void wake(void)
{
    const uint64_t one = 1;

    if (write(s_efd, &one, sizeof(one)) != sizeof(one)) {
        ESP_LOGW(TAG, "eventfd write failed");
    }
}

esp_err_t usbip_net_start(void)
{
    ESP_RETURN_ON_FALSE(!s_started, ESP_ERR_INVALID_STATE, TAG, "net start: already started");
    if (s_net == NULL) { // kept from a start that failed later on
        s_net = heap_caps_calloc(1, sizeof(*s_net), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        ESP_RETURN_ON_FALSE(s_net != NULL, ESP_ERR_NO_MEM, TAG, "net start: no memory for the state");
    }
    for (int i = 0; i < PENDING_MAX; i++) {
        s_net->pending[i].fd = -1;
    }
    s_net->sess.fd = -1;
    s_net->pub.state = USBIP_STATE_NO_DEVICE;
    s_rx_data = heap_caps_malloc(USBIP_XFER_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_tx = heap_caps_malloc(TX_RING_LEN, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_events = xQueueCreate(USBIP_USB_EVENTS_LEN, sizeof(usbip_event_t));
    s_efd = eventfd(0, 0);
    if (s_rx_data == NULL || s_tx == NULL || s_events == NULL || s_efd < 0) {
        ESP_LOGE(TAG, "net start: %s", s_efd < 0 ? "eventfd (is the eventfd VFS registered with room?)" : "no memory");
        heap_caps_free(s_rx_data);
        heap_caps_free(s_tx);
        s_rx_data = NULL;
        s_tx = NULL;
        if (s_events != NULL) {
            vQueueDelete(s_events);
            s_events = NULL;
        }
        if (s_efd >= 0) {
            close(s_efd);
            s_efd = -1;
        }
        return ESP_ERR_NO_MEM;
    }
    s_net->ops = *usbip_usb_ops();
    s_net->ops.emit = op_emit;
    s_net->ops.end_session = op_end_session;
    s_net->ops.now_ms = op_now_ms;
    s_started = true; // the queue and the eventfd stay: the USB side may hold them from here on
    usbip_usb_set_wake(wake);
    ESP_RETURN_ON_ERROR(usbip_usb_start(s_events), TAG, "usb start");
    // Created after the USB side: a device already there is reported by usbip_usb_start and waits in the queue.
    ESP_RETURN_ON_FALSE(xTaskCreatePinnedToCore(net_task, "usbip_net", TASK_STACK, NULL, TASK_PRIORITY, NULL,
                                                MGMT_CORE) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "network task");
    return ESP_OK;
}

void usbip_net_kick(void)
{
    if (!s_started) {
        return;
    }
    atomic_store(&s_kick, true);
    wake();
}
