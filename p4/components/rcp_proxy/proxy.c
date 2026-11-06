// RCP proxy: the rcp-proxy consumer of the radio mux and the proxy task. The mux's attach and detach (owner task) only
// leave a wish per transport and wake the task; the task alone opens and closes sessions and sockets, and waits in one
// select() over the adapters' sockets, the channels' eventfds and its kick eventfd (spec section RCP proxy mode).
#include "rcp_proxy.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "board_cores.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_eventfd.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "mdns.h"
#include "netcfg.h"
#include "rcp_proxy_priv.h"

static const char *TAG = "rcp_proxy";

#define TASK_PRIORITY 6
#define TASK_STACK 6144
// One session per transport: the C6 and the one open USB device. The mux detaches before it attaches again on the
// same transport, so a slot never has two channels.
#define SLOTS 2
#define SLOT_C6 0
#define SLOT_USB 1
// The wait is set by the back-off and lock timers; without one it is a long safety wake. Never shorter than 1 s.
#define WAIT_MIN_MS 1000
#define WAIT_MAX_MS 60000
#define EVENTS_PER_WAKE (RADIO_MUX_POOL_SLOTS + 4)
#define RAW_READS_PER_WAKE 16
#define MDNS_WAIT_MS 1000
#define SELECT_ERR_DELAY_MS 1000
#define SELECT_ERR_LOG_MS 60000
#define ENC_CAP RADIO_MUX_HDLC_ENCODED_MAX(RADIO_MUX_FRAME_MAX)
#define DEC_CAP (RADIO_MUX_FRAME_MAX + RADIO_MUX_HDLC_CRC_LEN)
#define PORT_TEXT_MAX 6

typedef struct {
    radio_mux_chan_t *ch; // NULL: no channel
    char id[RADIO_MUX_ID_MAX];
    bool frames; // radio_mux_chan_frames at attach
    radio_mux_proxy_settings_t set;
    uint32_t gen; // bumped by every attach and detach
    bool kick;
} want_t;

typedef struct {
    uint32_t gen; // the wish the session was opened for
    bool active;
    proxy_state_t state;
    char peer[RADIO_MUX_PEER_MAX];
} pub_t;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static want_t s_want[SLOTS]; // under s_lock: attach, detach, kick
static pub_t s_pub[SLOTS];   // under s_lock: what the task last published
static int s_kick_fd = -1;
static bool s_registered;
static bool s_task_up; // false: attach is refused (the task could not be created)

// Proxy task only.
static proxy_t s_px[SLOTS];
static uint32_t s_gen[SLOTS];
static uint8_t s_rx[RCP_PROXY_RX_MAX];
static uint8_t s_raw[RCP_PROXY_UDP_RAW_MAX];

/* ---- helpers for the adapters ---- */

uint8_t *rcp_proxy_rx_buf(void)
{
    return s_rx;
}

int64_t rcp_proxy_now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

void rcp_proxy_set_nonblocking(int fd)
{
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
}

void rcp_proxy_addr_from(const struct sockaddr *sa, rcp_proxy_addr_t *out)
{
    memset(out, 0, sizeof(*out));
    if (sa->sa_family == AF_INET) {
        const struct sockaddr_in *v4 = (const struct sockaddr_in *)sa;

        out->ip[10] = 0xff;
        out->ip[11] = 0xff;
        memcpy(&out->ip[12], &v4->sin_addr.s_addr, 4);
        out->port = ntohs(v4->sin_port);
    } else if (sa->sa_family == AF_INET6) {
        const struct sockaddr_in6 *v6 = (const struct sockaddr_in6 *)sa;

        memcpy(out->ip, v6->sin6_addr.s6_addr, sizeof(out->ip));
        out->port = ntohs(v6->sin6_port);
        out->scope = v6->sin6_scope_id;
    }
}

// "a.b.c.d:port" (IPv4, also mapped on a dual-stack socket) or "[addr]:port".
static void peer_text(const struct sockaddr *sa, char *out, size_t cap)
{
    rcp_proxy_addr_t a;
    char ip[INET6_ADDRSTRLEN] = "";
    static const uint8_t mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};

    rcp_proxy_addr_from(sa, &a);
    if (memcmp(a.ip, mapped, sizeof(mapped)) == 0) {
        inet_ntop(AF_INET, &a.ip[12], ip, sizeof(ip));
        snprintf(out, cap, "%s:%u", ip, a.port);
    } else {
        inet_ntop(AF_INET6, a.ip, ip, sizeof(ip));
        snprintf(out, cap, "[%s]:%u", ip, a.port);
    }
}

static esp_err_t resolve_host(proxy_t *p, int socktype)
{
    char name[RADIO_MUX_PROXY_HOST_MAX];

    memset(&p->dst, 0, sizeof(p->dst));
    p->dst_len = 0;
    if (rcp_proxy_host_mdns(p->set.host, name, sizeof(name))) {
        esp_ip4_addr_t a4;
        esp_ip6_addr_t a6;

        if (mdns_query_a(name, MDNS_WAIT_MS, &a4) == ESP_OK && a4.addr != 0) {
            struct sockaddr_in *v4 = (struct sockaddr_in *)&p->dst;

            v4->sin_family = AF_INET;
            v4->sin_port = htons(p->set.port);
            v4->sin_addr.s_addr = a4.addr;
            p->dst_len = sizeof(*v4);
            return ESP_OK;
        }
        if (mdns_query_aaaa(name, MDNS_WAIT_MS, &a6) == ESP_OK) {
            struct sockaddr_in6 *v6 = (struct sockaddr_in6 *)&p->dst;

            v6->sin6_family = AF_INET6;
            v6->sin6_port = htons(p->set.port);
            memcpy(v6->sin6_addr.s6_addr, a6.addr, sizeof(v6->sin6_addr.s6_addr));
            v6->sin6_scope_id = a6.zone;
            p->dst_len = sizeof(*v6);
            return ESP_OK;
        }
        return ESP_ERR_NOT_FOUND;
    }
    char port[PORT_TEXT_MAX];
    const struct addrinfo hints = {.ai_family = AF_UNSPEC, .ai_socktype = socktype};
    struct addrinfo *res = NULL;

    snprintf(port, sizeof(port), "%u", p->set.port);
    if (getaddrinfo(p->set.host, port, &hints, &res) != 0 || !res) {
        return ESP_ERR_NOT_FOUND;
    }
    const size_t len = res->ai_addrlen < sizeof(p->dst) ? res->ai_addrlen : sizeof(p->dst);

    memcpy(&p->dst, res->ai_addr, len);
    p->dst_len = (socklen_t)len;
    freeaddrinfo(res);
    return ESP_OK;
}

esp_err_t rcp_proxy_resolve(proxy_t *p, int socktype)
{
    const esp_err_t err = resolve_host(p, socktype);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s: %s does not resolve", p->id, p->set.host);
        return err;
    }
    // One rule for both adapters: the connect target is judged like a UDP source (the mesh is refused).
    if (!netcfg_peer_allowed((const struct sockaddr *)&p->dst, NETCFG_SVC_RCP_PROXY)) {
        ESP_LOGW(TAG, "%s: %s is not allowed by the network access rules", p->id, p->set.host);
        return ESP_ERR_NOT_ALLOWED;
    }
    return ESP_OK;
}

/* ---- session ---- */

static const char *state_name(proxy_state_t st)
{
    switch (st) {
    case PROXY_LISTENING:
        return "listening";
    case PROXY_CONNECTING:
        return "connecting";
    case PROXY_BACKOFF:
        return "backoff";
    case PROXY_CONNECTED:
        return "connected";
    case PROXY_IDLE:
    default:
        return NULL;
    }
}

static void free_bufs(proxy_t *p)
{
    free(p->ring_buf);
    free(p->enc_buf);
    free(p->dec_buf);
    free(p->cut_buf);
    p->ring_buf = p->enc_buf = p->dec_buf = p->cut_buf = NULL;
}

// The buffers this session needs: a ring for a stream peer, encode and decode buffers on a frame channel, the
// cutter on a byte channel over datagrams with framing hdlc.
static esp_err_t alloc_bufs(proxy_t *p)
{
    if (p->ring_buf || p->enc_buf || p->cut_buf) {
        return ESP_OK; // allocated by an earlier try
    }
    bool ok = true;

    if (!p->ad->datagram) {
        p->ring_buf = malloc(RCP_PROXY_RING_MIN);
        ok = ok && p->ring_buf;
    }
    if (p->frames) {
        p->enc_buf = malloc(ENC_CAP);
        p->dec_buf = malloc(DEC_CAP);
        ok = ok && p->enc_buf && p->dec_buf;
    } else if (p->ad->datagram && p->set.framing == RADIO_MUX_PROXY_HDLC) {
        p->cut_buf = malloc(ENC_CAP);
        ok = ok && p->cut_buf;
    }
    if (!ok) {
        free_bufs(p);
        return ESP_ERR_NO_MEM;
    }
    if (p->ring_buf) {
        rcp_proxy_ring_init(&p->ring, p->ring_buf, RCP_PROXY_RING_MIN);
    }
    if (p->dec_buf) {
        radio_mux_hdlc_dec_init(&p->dec, p->dec_buf, DEC_CAP);
    }
    if (p->cut_buf) {
        rcp_proxy_cut_init(&p->cut, p->cut_buf, ENC_CAP);
    }
    return ESP_OK;
}

void rcp_proxy_retry(proxy_t *p, const char *why)
{
    const int64_t now = rcp_proxy_now_ms();
    const int64_t lasted = p->state == PROXY_CONNECTED ? now - p->up_ms : 0;

    p->ad->close(p);
    p->backoff_s = rcp_proxy_backoff_next(p->backoff_s, lasted);
    p->retry_ms = now + (int64_t)p->backoff_s * 1000;
    p->state = PROXY_BACKOFF;
    p->peer[0] = '\0';
    radio_mux_set_busy_peer(p->ch, false);
    ESP_LOGW(TAG, "%s: %s; again in %lu s", p->id, why, (unsigned long)p->backoff_s);
}

static void open_adapter(proxy_t *p)
{
    if (alloc_bufs(p) != ESP_OK) {
        rcp_proxy_retry(p, "out of memory for the session buffers");
        return;
    }
    const esp_err_t err = p->ad->open(p);

    if (err != ESP_OK) {
        rcp_proxy_retry(p, esp_err_to_name(err));
    }
}

static void session_open(proxy_t *p, const want_t *w)
{
    memset(p, 0, sizeof(*p));
    p->lfd = -1;
    p->fd = -1;
    p->wake_ms = -1;
    p->ch = w->ch;
    strlcpy(p->id, w->id, sizeof(p->id));
    p->set = w->set;
    p->frames = w->frames;
    p->ad = p->set.adapter == RADIO_MUX_PROXY_UDP ? &rcp_proxy_adapter_udp : &rcp_proxy_adapter_tcp;
    ESP_LOGI(TAG, "%s: %s %s %s%s%u%s", p->id, p->set.dir == RADIO_MUX_PROXY_CONNECT ? "connect to" : "listen on",
             p->ad->name, p->set.dir == RADIO_MUX_PROXY_CONNECT ? p->set.host : "port ",
             p->set.dir == RADIO_MUX_PROXY_CONNECT ? ":" : "", p->set.port,
             p->ad->datagram ? (p->set.framing == RADIO_MUX_PROXY_RAW ? ", framing raw" : ", framing hdlc") : "");
    open_adapter(p);
}

static void session_close(proxy_t *p)
{
    if (!p->ch) {
        return;
    }
    p->ad->close(p);
    radio_mux_set_busy_peer(p->ch, false);
    ESP_LOGI(TAG, "%s: closed (to radio %llu B, to peer %llu B, discarded %lu B, refused %lu, overflows %lu, "
             "radio tx errors %lu)", p->id, (unsigned long long)p->to_radio, (unsigned long long)p->to_peer,
             (unsigned long)p->discarded, (unsigned long)p->refused, (unsigned long)p->overflows,
             (unsigned long)p->tx_errors);
    free_bufs(p);
    memset(p, 0, sizeof(*p));
    p->lfd = -1;
    p->fd = -1;
    p->wake_ms = -1;
}

void rcp_proxy_peer_up(proxy_t *p, const struct sockaddr *peer, bool new_session)
{
    peer_text(peer, p->peer, sizeof(p->peer));
    p->state = PROXY_CONNECTED;
    radio_mux_set_busy_peer(p->ch, true); // a C6 job is refused while a peer is connected (409 c6_busy)
    p->up_ms = rcp_proxy_now_ms();
    if (p->ring_buf) {
        rcp_proxy_ring_clear(&p->ring); // a new session starts clean
    }
    if (p->cut_buf) {
        rcp_proxy_cut_clear(&p->cut);
    }
    if (p->dec_buf) {
        radio_mux_hdlc_dec_init(&p->dec, p->dec_buf, DEC_CAP);
    }
    ESP_LOGI(TAG, "%s: peer %s (%s)%s", p->id, p->peer, p->ad->name, new_session ? "" : ", same peer again");
    if (p->frames && new_session) { // the peer's host starts from the radio's reset handshake
        const esp_err_t err = radio_mux_reset(p->ch);

        if (err != ESP_OK) {
            ESP_LOGW(TAG, "%s: radio reset for the new peer: %s", p->id, esp_err_to_name(err));
        }
    }
}

void rcp_proxy_peer_lost(proxy_t *p, proxy_state_t st)
{
    p->state = st;
    p->peer[0] = '\0';
    radio_mux_set_busy_peer(p->ch, false);
    if (p->ring_buf) {
        rcp_proxy_ring_clear(&p->ring);
    }
}

/* ---- data paths ---- */

void rcp_proxy_from_peer(proxy_t *p, const uint8_t *d, size_t n)
{
    if (!p->frames) {
        if (radio_mux_tx(p->ch, d, n) == ESP_OK) {
            p->to_radio += n;
        } else { // no radio right now: never replayed into a radio that comes back
            p->tx_errors++;
            p->discarded += n;
        }
        return;
    }
    while (n > 0) {
        bool done = false;
        const size_t used = radio_mux_hdlc_decode_feed(&p->dec, d, n, &done);

        d += used;
        n -= used;
        if (done) {
            if (radio_mux_tx(p->ch, p->dec.buf, p->dec.len) == ESP_OK) {
                p->to_radio += p->dec.len;
            } else {
                p->tx_errors++;
                p->discarded += p->dec.len;
            }
            radio_mux_hdlc_dec_set_buf(&p->dec, p->dec_buf, DEC_CAP);
        } else if (used == 0) {
            break;
        }
    }
}

static void to_peer(proxy_t *p, const uint8_t *d, size_t n)
{
    if (p->ad->send(p, d, n) == ESP_OK) {
        p->to_peer += n;
    } else {
        p->discarded += n;
    }
}

static void emit_datagram(void *ctx, const uint8_t *frame, size_t len)
{
    to_peer(ctx, frame, len);
}

// A byte channel's bytes: as they are on a stream or with framing raw (a read is at most RCP_PROXY_UDP_RAW_MAX), cut
// into HDLC frames for datagrams with framing hdlc.
static void bytes_to_peer(proxy_t *p, const uint8_t *d, size_t n)
{
    if (!p->cut_buf) {
        to_peer(p, d, n);
        return;
    }
    const uint32_t before = p->cut.overflows;

    rcp_proxy_cut_feed(&p->cut, d, n, emit_datagram, p);
    if (p->cut.overflows != before) {
        p->overflows += p->cut.overflows - before;
        ESP_LOGW(TAG, "%s: no HDLC frame in %u bytes: dropped (%lu so far)", p->id, (unsigned)ENC_CAP,
                 (unsigned long)p->cut.overflows);
    }
}

// A frame channel's frame, HDLC-encoded: one datagram (framing hdlc), chunks (framing raw) or stream bytes.
static void frame_to_peer(proxy_t *p, const uint8_t *frame, size_t n)
{
    const size_t m = p->enc_buf ? radio_mux_hdlc_encode(frame, n, p->enc_buf, ENC_CAP) : 0;

    if (m == 0) { // no buffer (backing off after an allocation failure) or a frame above RADIO_MUX_FRAME_MAX
        p->discarded += n;
        return;
    }
    if (!p->ad->datagram || p->set.framing == RADIO_MUX_PROXY_HDLC) {
        to_peer(p, p->enc_buf, m);
        return;
    }
    for (size_t off = 0; off < m; off += RCP_PROXY_UDP_RAW_MAX) {
        const size_t left = m - off;

        to_peer(p, p->enc_buf + off, left < RCP_PROXY_UDP_RAW_MAX ? left : RCP_PROXY_UDP_RAW_MAX);
    }
}

// The channel's eventfd is readable: take every event (GONE, RESET, frames), then the raw bytes. Both calls clear the
// eventfd once nothing is left, so a later wait does not wake for nothing; a capped round leaves it set.
static void radio_in(proxy_t *p)
{
    radio_mux_ev_t ev;

    for (int i = 0; i < EVENTS_PER_WAKE && p->ch && radio_mux_poll(p->ch, &ev); i++) {
        switch (ev.type) {
        case RADIO_MUX_EV_RX_FRAME:
            if (p->frames) {
                frame_to_peer(p, ev.slot, ev.len);
            }
            radio_mux_release(p->ch, ev.slot);
            break;
        case RADIO_MUX_EV_RESET:
            ESP_LOGI(TAG, "%s: the radio was reset", p->id);
            break;
        case RADIO_MUX_EV_GONE:
            ESP_LOGI(TAG, "%s: the channel is gone: peer dropped", p->id);
            session_close(p); // opened again when the mux attaches the channel again
            return;
        }
    }
    for (int i = 0; i < RAW_READS_PER_WAKE && p->ch && !p->frames; i++) {
        const size_t n = radio_mux_read_raw(p->ch, s_raw, sizeof(s_raw));

        if (n == 0) {
            break;
        }
        bytes_to_peer(p, s_raw, n);
    }
}

/* ---- the task ---- */

static void poke(void)
{
    const uint64_t one = 1;

    if (s_kick_fd >= 0 && write(s_kick_fd, &one, sizeof(one)) != sizeof(one)) {
        ESP_LOGW(TAG, "kick eventfd write failed");
    }
}

// Applies the wishes of attach, detach and kick: a new generation closes the session and opens the wished one.
static void reconcile(void)
{
    for (int i = 0; i < SLOTS; i++) {
        want_t w;

        taskENTER_CRITICAL(&s_lock);
        w = s_want[i];
        s_want[i].kick = false;
        taskEXIT_CRITICAL(&s_lock);
        proxy_t *p = &s_px[i];

        if (w.gen != s_gen[i]) {
            session_close(p);
            s_gen[i] = w.gen;
            if (w.ch) {
                session_open(p, &w);
            }
        } else if (w.kick && p->ch && p->state != PROXY_BACKOFF) {
            ESP_LOGI(TAG, "%s: kicked", p->id);
            p->ad->drop(p);
        }
    }
}

static void publish(void)
{
    for (int i = 0; i < SLOTS; i++) {
        const proxy_t *p = &s_px[i];

        taskENTER_CRITICAL(&s_lock);
        s_pub[i].gen = s_gen[i];
        s_pub[i].active = p->ch != NULL;
        s_pub[i].state = p->state;
        memcpy(s_pub[i].peer, p->peer, sizeof(s_pub[i].peer));
        taskEXIT_CRITICAL(&s_lock);
    }
}

static int64_t earliest(int64_t a, int64_t b)
{
    if (a < 0) {
        return b;
    }
    return (b < 0 || a < b) ? a : b;
}

static void proxy_task(void *arg)
{
    int64_t select_err_log_ms = -SELECT_ERR_LOG_MS;

    for (;;) {
        reconcile();
        const int64_t now = rcp_proxy_now_ms();
        fd_set rd, wr;
        int maxfd = s_kick_fd;
        int64_t wake = -1;

        FD_ZERO(&rd);
        FD_ZERO(&wr);
        FD_SET(s_kick_fd, &rd);
        for (int i = 0; i < SLOTS; i++) {
            proxy_t *p = &s_px[i];

            if (!p->ch) {
                continue;
            }
            if (p->state == PROXY_BACKOFF && now >= p->retry_ms) {
                open_adapter(p);
            }
            const int efd = radio_mux_eventfd(p->ch);

            if (efd >= 0) {
                FD_SET(efd, &rd);
                maxfd = efd > maxfd ? efd : maxfd;
            }
            if (p->state == PROXY_BACKOFF) {
                wake = earliest(wake, p->retry_ms);
                continue;
            }
            p->wake_ms = -1;
            maxfd = p->ad->fds(p, &rd, &wr, maxfd);
            wake = earliest(wake, p->wake_ms);
        }
        publish();
        int64_t wait_ms = wake < 0 ? WAIT_MAX_MS : wake - now;

        wait_ms = wait_ms < WAIT_MIN_MS ? WAIT_MIN_MS : (wait_ms > WAIT_MAX_MS ? WAIT_MAX_MS : wait_ms);
        struct timeval tv = {.tv_sec = wait_ms / 1000, .tv_usec = (wait_ms % 1000) * 1000};
        const int ready = select(maxfd + 1, &rd, &wr, NULL, &tv);

        if (ready < 0) {
            const int64_t t = rcp_proxy_now_ms();

            if (t - select_err_log_ms >= SELECT_ERR_LOG_MS) {
                ESP_LOGE(TAG, "select: errno %d", errno);
                select_err_log_ms = t;
            }
            vTaskDelay(pdMS_TO_TICKS(SELECT_ERR_DELAY_MS));
            continue;
        }
        if (ready == 0) {
            FD_ZERO(&rd);
            FD_ZERO(&wr);
        }
        if (FD_ISSET(s_kick_fd, &rd)) {
            uint64_t v;

            if (read(s_kick_fd, &v, sizeof(v)) != sizeof(v)) {
                ESP_LOGW(TAG, "kick eventfd read failed");
            }
            // A detach (and the next attach) comes before the next consumer's traffic on the same eventfd: apply it
            // first, then wait again with fresh sets (every readiness is level-triggered, nothing is lost).
            reconcile();
            continue;
        }
        for (int i = 0; i < SLOTS; i++) {
            proxy_t *p = &s_px[i];

            if (!p->ch) {
                continue;
            }
            const int efd = radio_mux_eventfd(p->ch);

            if (efd >= 0 && FD_ISSET(efd, &rd)) {
                radio_in(p);
            }
            if (p->ch && p->state != PROXY_BACKOFF) {
                p->ad->ready(p, &rd, &wr);
            }
        }
    }
}

/* ---- consumer ---- */

static int slot_of(const radio_mux_chan_t *ch)
{
    return strcmp(radio_mux_chan_id(ch), RADIO_MUX_C6_ID) == 0 ? SLOT_C6 : SLOT_USB;
}

// Owner task: note the wish and wake the proxy task. The settings are read here, at attach: a change re-attaches.
static esp_err_t on_attach(radio_mux_chan_t *ch, void *ctx)
{
    radio_mux_proxy_settings_t set;

    if (!s_task_up) {
        return ESP_ERR_INVALID_STATE;
    }
    radio_mux_get_proxy(ch, &set);
    if (rcp_proxy_settings_check(&set) != RCP_PROXY_OK) {
        ESP_LOGE(TAG, "%s: stored rcp_proxy settings are invalid", radio_mux_chan_id(ch));
        return ESP_ERR_INVALID_ARG;
    }
    const int i = slot_of(ch);

    taskENTER_CRITICAL(&s_lock);
    s_want[i].ch = ch;
    strlcpy(s_want[i].id, radio_mux_chan_id(ch), sizeof(s_want[i].id));
    s_want[i].frames = radio_mux_chan_frames(ch); // the mux owns the delivery kind (the C6: always frames)
    s_want[i].set = set;
    s_want[i].gen++;
    s_want[i].kick = false;
    taskEXIT_CRITICAL(&s_lock);
    poke();
    return ESP_OK;
}

static void on_detach(radio_mux_chan_t *ch, void *ctx)
{
    const int i = slot_of(ch);

    taskENTER_CRITICAL(&s_lock);
    if (s_want[i].ch == ch) {
        s_want[i].ch = NULL;
        s_want[i].id[0] = '\0';
        s_want[i].gen++;
        s_want[i].kick = false;
    }
    taskEXIT_CRITICAL(&s_lock);
    poke();
}

// The consumer first, then the task: a refused registration leaves nothing running. A task that cannot be created
// leaves the consumer registered with attach refused (its channels show the fault).
esp_err_t rcp_proxy_register(void)
{
    ESP_RETURN_ON_FALSE(!s_registered, ESP_ERR_INVALID_STATE, TAG, "already registered");
    s_kick_fd = eventfd(0, 0);
    ESP_RETURN_ON_FALSE(s_kick_fd >= 0, ESP_ERR_NO_MEM, TAG, "eventfd (is the eventfd VFS registered?)");
    const radio_mux_consumer_t c = {
        .mode = RADIO_MUX_MODE_RCP_PROXY,
        .needs = RADIO_MUX_FW_UNKNOWN,
        .limit = 0,
        .reboot_on_change = false,
        .frames = false, // USB channels: raw bytes; the C6 always delivers frames (radio_mux_chan_frames)
        .attach = on_attach,
        .detach = on_detach,
        .ctx = NULL,
    };
    const esp_err_t err = radio_mux_register_consumer(&c);

    if (err != ESP_OK) {
        close(s_kick_fd);
        s_kick_fd = -1;
        return err;
    }
    s_registered = true;
    if (xTaskCreatePinnedToCore(proxy_task, "rcp_proxy", TASK_STACK, NULL, TASK_PRIORITY, NULL, MGMT_CORE) != pdPASS) {
        ESP_LOGE(TAG, "the proxy task could not be created: rcp-proxy channels stay in fault");
        return ESP_ERR_NO_MEM;
    }
    s_task_up = true;
    return ESP_OK;
}

void rcp_proxy_kick(const char *channel_id)
{
    bool found = false;

    if (!channel_id) {
        return;
    }
    taskENTER_CRITICAL(&s_lock);
    for (int i = 0; i < SLOTS; i++) {
        if (s_want[i].ch && strcmp(s_want[i].id, channel_id) == 0) {
            s_want[i].kick = true;
            found = true;
        }
    }
    taskEXIT_CRITICAL(&s_lock);
    if (found) {
        poke();
    }
}

esp_err_t rcp_proxy_get_status(const char *channel_id, rcp_proxy_status_t *out)
{
    esp_err_t err = ESP_ERR_NOT_FOUND;

    if (!channel_id || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    taskENTER_CRITICAL(&s_lock);
    for (int i = 0; i < SLOTS; i++) {
        const want_t *w = &s_want[i];
        const pub_t *pub = &s_pub[i];

        if (!w->ch || strcmp(w->id, channel_id) != 0) {
            continue;
        }
        if (pub->active && pub->gen == w->gen && state_name(pub->state)) {
            out->state = state_name(pub->state);
            memcpy(out->peer, pub->peer, sizeof(out->peer));
        } else { // attached, the task has not opened it yet
            out->state = w->set.dir == RADIO_MUX_PROXY_CONNECT ? "connecting" : "listening";
            out->peer[0] = '\0';
        }
        err = ESP_OK;
        break;
    }
    taskEXIT_CRITICAL(&s_lock);
    return err;
}
