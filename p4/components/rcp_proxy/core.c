// Decisions of the RCP proxy: the settings check, the connect back-off, the UDP peer lock, the accept rule, the byte
// ring (moved from usb_serial) and the HDLC cutter of UDP framing. Pure: no ESP-IDF calls.
#include "rcp_proxy_core.h"

#include <string.h>

#include "radio_mux_hdlc.h"

#define PORT_WEB 80
#define PORT_BGP 179
#define HOST_NAME_MAX_LEN 63
#define LABEL_MAX_LEN 63
#define IP4_PARTS 4
#define IP4_PART_MAX 255
#define IP6_GROUPS 8
#define IP6_GROUP_DIGITS 4
#define MDNS_SUFFIX ".local"

/* ---- settings ---- */

static bool is_digit(char c)
{
    return c >= '0' && c <= '9';
}

static bool is_hex(char c)
{
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static bool is_alnum(char c)
{
    return is_digit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

// Dotted quad of n bytes at s: four decimal parts 0..255, one to three digits each.
static bool ip4_ok(const char *s, size_t n)
{
    size_t parts = 0;
    size_t i = 0;

    while (parts < IP4_PARTS) {
        unsigned value = 0;
        size_t digits = 0;

        while (i < n && is_digit(s[i]) && digits < 3) {
            value = value * 10 + (unsigned)(s[i] - '0');
            digits++;
            i++;
        }
        if (digits == 0 || value > IP4_PART_MAX) {
            return false;
        }
        parts++;
        if (parts < IP4_PARTS) {
            if (i >= n || s[i] != '.') {
                return false;
            }
            i++;
        }
    }
    return i == n;
}

// RFC 4291 text form: up to eight groups of one to four hex digits, one "::" for a run of zero groups, an optional
// dotted-quad tail counting as two groups. No zone.
static bool ip6_ok(const char *s)
{
    const size_t n = strlen(s);
    size_t groups = 0;
    bool gap = false;
    size_t i = 0;

    if (n >= 2 && s[0] == ':' && s[1] == ':') {
        gap = true;
        i = 2;
    } else if (n >= 1 && s[0] == ':') {
        return false;
    }
    while (i < n) {
        size_t digits = 0;
        const size_t start = i;

        while (i < n && is_hex(s[i])) {
            digits++;
            i++;
        }
        if (i < n && s[i] == '.') { // an IPv4 tail ends the address
            if (!ip4_ok(&s[start], n - start)) {
                return false;
            }
            groups += 2;
            i = n;
            break;
        }
        if (digits == 0 || digits > IP6_GROUP_DIGITS) {
            return false;
        }
        groups++;
        if (i == n) {
            break;
        }
        if (s[i] != ':') {
            return false;
        }
        i++;
        if (i < n && s[i] == ':') {
            if (gap) {
                return false; // a second "::"
            }
            gap = true;
            i++;
        } else if (i == n) {
            return false; // a trailing single ':'
        }
    }
    return gap ? groups < IP6_GROUPS : groups == IP6_GROUPS;
}

// Labels of letters, digits and inner hyphens, 1..63 each; the last label is not all digits (it would be a broken
// IPv4 literal). A trailing dot is not accepted.
static bool name_ok(const char *s, size_t n)
{
    size_t label = 0;
    bool all_digits = true;

    for (size_t i = 0; i <= n; i++) {
        const char c = i < n ? s[i] : '.';

        if (c == '.') {
            if (label == 0 || label > LABEL_MAX_LEN || s[i - 1] == '-') {
                return false;
            }
            if (i == n && all_digits) {
                return false;
            }
            label = 0;
            all_digits = true;
            continue;
        }
        if (!is_alnum(c) && c != '-') {
            return false;
        }
        if (c == '-' && label == 0) {
            return false;
        }
        all_digits = all_digits && is_digit(c);
        label++;
    }
    return true;
}

static bool host_ok(const char *host, size_t n)
{
    if (n == 0 || n > HOST_NAME_MAX_LEN) {
        return false;
    }
    if (strchr(host, ':')) {
        return ip6_ok(host);
    }
    return ip4_ok(host, n) || name_ok(host, n);
}

rcp_proxy_check_t rcp_proxy_settings_check(const radio_mux_proxy_settings_t *s)
{
    if (s->port == 0 || s->port == PORT_WEB || s->port == PORT_BGP) {
        return RCP_PROXY_BAD_PORT;
    }
    const size_t n = strnlen(s->host, sizeof(s->host));

    if (n == sizeof(s->host)) {
        return RCP_PROXY_BAD_HOST; // no terminator
    }
    if (s->dir == RADIO_MUX_PROXY_CONNECT && n == 0) {
        return RCP_PROXY_NEED_HOST;
    }
    if (n > 0 && !host_ok(s->host, n)) {
        return RCP_PROXY_BAD_HOST;
    }
    return RCP_PROXY_OK;
}

bool rcp_proxy_host_mdns(const char *host, char *out, size_t cap)
{
    const size_t n = strlen(host);
    const size_t suffix = sizeof(MDNS_SUFFIX) - 1;

    if (n <= suffix || n - suffix >= cap) {
        return false;
    }
    for (size_t i = 0; i < suffix; i++) {
        if (lower(host[n - suffix + i]) != MDNS_SUFFIX[i]) {
            return false;
        }
    }
    memcpy(out, host, n - suffix);
    out[n - suffix] = '\0';
    return true;
}

/* ---- connect back-off ---- */

uint32_t rcp_proxy_backoff_next(uint32_t prev_s, int64_t connected_for_ms)
{
    if (prev_s == 0 || connected_for_ms >= RCP_PROXY_BACKOFF_RESET_MS) {
        return 1;
    }
    return prev_s >= RCP_PROXY_BACKOFF_MAX_S / 2 ? RCP_PROXY_BACKOFF_MAX_S : prev_s * 2;
}

/* ---- UDP peer lock ---- */

bool rcp_proxy_addr_eq(const rcp_proxy_addr_t *a, const rcp_proxy_addr_t *b)
{
    return a->port == b->port && a->scope == b->scope && memcmp(a->ip, b->ip, sizeof(a->ip)) == 0;
}

void rcp_proxy_udp_lock_init(rcp_proxy_udp_lock_t *lock)
{
    memset(lock, 0, sizeof(*lock));
}

bool rcp_proxy_udp_lock_expire(rcp_proxy_udp_lock_t *lock, int64_t now_ms)
{
    if (!lock->locked || now_ms - lock->last_ms < RCP_PROXY_UDP_SILENCE_MS) {
        return false;
    }
    lock->locked = false;
    return true;
}

bool rcp_proxy_udp_lock_offer(rcp_proxy_udp_lock_t *lock, const rcp_proxy_addr_t *src, int64_t now_ms)
{
    rcp_proxy_udp_lock_expire(lock, now_ms);
    if (lock->locked && !rcp_proxy_addr_eq(&lock->peer, src)) {
        lock->refused++;
        return false;
    }
    lock->locked = true;
    lock->peer = *src;
    lock->last_ms = now_ms;
    return true;
}

void rcp_proxy_udp_lock_kick(rcp_proxy_udp_lock_t *lock)
{
    lock->locked = false;
}

int64_t rcp_proxy_udp_lock_deadline(const rcp_proxy_udp_lock_t *lock)
{
    return lock->locked ? lock->last_ms + RCP_PROXY_UDP_SILENCE_MS : -1;
}

/* ---- new peer session (reset-handshake replay) ---- */

void rcp_proxy_peer_memo_init(rcp_proxy_peer_memo_t *m)
{
    memset(m, 0, sizeof(*m));
}

bool rcp_proxy_peer_new(rcp_proxy_peer_memo_t *m, const rcp_proxy_addr_t *peer)
{
    const bool fresh = !m->had || !rcp_proxy_addr_eq(&m->last, peer);

    m->had = true;
    m->last = *peer;
    return fresh;
}

/* ---- TCP accept rule ---- */

rcp_proxy_accept_t rcp_proxy_accept(bool peer_connected)
{
    return peer_connected ? RCP_PROXY_REFUSE : RCP_PROXY_TAKE;
}

/* ---- byte ring ---- */

void rcp_proxy_ring_init(rcp_proxy_ring_t *r, uint8_t *storage, size_t cap)
{
    r->data = storage;
    r->cap = cap;
    r->head = 0;
    r->len = 0;
}

bool rcp_proxy_ring_push(rcp_proxy_ring_t *r, const uint8_t *src, size_t n)
{
    if (n > r->cap - r->len) {
        return false;
    }
    const size_t tail = (r->head + r->len) % r->cap;
    size_t first = r->cap - tail;

    if (first > n) {
        first = n;
    }
    memcpy(&r->data[tail], src, first);
    memcpy(r->data, src + first, n - first);
    r->len += n;
    return true;
}

size_t rcp_proxy_ring_take(rcp_proxy_ring_t *r, uint8_t *out, size_t cap)
{
    const size_t n = r->len < cap ? r->len : cap;
    size_t first = r->cap - r->head;

    if (first > n) {
        first = n;
    }
    memcpy(out, &r->data[r->head], first);
    memcpy(out + first, r->data, n - first);
    r->head = (r->head + n) % r->cap;
    r->len -= n;
    return n;
}

size_t rcp_proxy_ring_peek(const rcp_proxy_ring_t *r, const uint8_t **p)
{
    const size_t run = r->cap - r->head;

    *p = &r->data[r->head];
    return r->len < run ? r->len : run;
}

void rcp_proxy_ring_drop(rcp_proxy_ring_t *r, size_t n)
{
    if (n > r->len) {
        n = r->len;
    }
    r->head = (r->head + n) % r->cap;
    r->len -= n;
}

void rcp_proxy_ring_clear(rcp_proxy_ring_t *r)
{
    r->head = 0;
    r->len = 0;
}

/* ---- HDLC cutter (UDP framing hdlc on a byte channel) ---- */

void rcp_proxy_cut_init(rcp_proxy_cut_t *c, uint8_t *buf, size_t cap)
{
    c->buf = buf;
    c->cap = cap;
    c->len = 0;
    c->overflows = 0;
}

void rcp_proxy_cut_clear(rcp_proxy_cut_t *c)
{
    c->len = 0;
}

// Emits every complete frame in the buffer and moves what is left to the front.
static size_t cut_emit(rcp_proxy_cut_t *c, rcp_proxy_emit_fn emit, void *ctx)
{
    size_t frames = 0;
    size_t off = 0;
    size_t frame_len;
    size_t drop;

    while ((drop = radio_mux_hdlc_chunk(c->buf + off, c->len - off, &frame_len)) > 0) {
        emit(ctx, c->buf + off + drop - frame_len + 1, frame_len);
        frames++;
        off += drop;
    }
    if (off > 0) {
        memmove(c->buf, c->buf + off, c->len - off);
        c->len -= off;
    }
    return frames;
}

size_t rcp_proxy_cut_feed(rcp_proxy_cut_t *c, const uint8_t *in, size_t n, rcp_proxy_emit_fn emit, void *ctx)
{
    size_t frames = 0;

    while (n > 0) {
        size_t room = c->cap - c->len;

        if (room == 0) { // full without a complete frame
            const bool flag_last = c->buf[c->len - 1] == RADIO_MUX_HDLC_FLAG;

            c->buf[0] = RADIO_MUX_HDLC_FLAG;
            c->len = flag_last ? 1 : 0;
            c->overflows++;
            room = c->cap - c->len;
        }
        const size_t take = n < room ? n : room;

        memcpy(c->buf + c->len, in, take);
        c->len += take;
        in += take;
        n -= take;
        frames += cut_emit(c, emit, ctx);
    }
    return frames;
}
