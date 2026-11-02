#include "render.h"

#include <string.h>

#include "esp_check.h"

#define CHUNK_SIZE 2048

static const char *TAG = "mesh_scan";

typedef struct {
    char buf[CHUNK_SIZE];
    size_t len;
    mesh_scan_sink_t sink;
    void *ctx;
    esp_err_t err; /**< first sink error; once set, nothing more is written */
} writer_t;

static void flush_buffer(writer_t *w)
{
    if (w->err == ESP_OK && w->len > 0) {
        w->err = w->sink(w->ctx, w->buf, w->len);
    }
    w->len = 0;
}

static void put(writer_t *w, const char *text, size_t n)
{
    while (n > 0 && w->err == ESP_OK) {
        if (w->len == sizeof(w->buf)) {
            flush_buffer(w);
            continue;
        }
        const size_t room = sizeof(w->buf) - w->len;
        const size_t take = n < room ? n : room;

        memcpy(w->buf + w->len, text, take);
        w->len += take;
        text += take;
        n -= take;
    }
}

static void raw(writer_t *w, const char *text)
{
    put(w, text, strlen(text));
}

static void u64(writer_t *w, uint64_t value)
{
    char digits[20];
    size_t n = 0;

    do {
        digits[sizeof(digits) - 1 - n++] = (char)('0' + value % 10);
        value /= 10;
    } while (value > 0);
    put(w, digits + sizeof(digits) - n, n);
}

static void i64(writer_t *w, int64_t value)
{
    if (value < 0) {
        raw(w, "-");
        u64(w, (uint64_t)0 - (uint64_t)value);
    } else {
        u64(w, (uint64_t)value);
    }
}

static void boolean(writer_t *w, bool value)
{
    raw(w, value ? "true" : "false");
}

static void comma(writer_t *w, size_t index)
{
    if (index > 0) {
        raw(w, ",");
    }
}

/** Length of the valid UTF-8 sequence at `p` (at most `avail` bytes left), 0 if there is none. */
static size_t utf8_len(const uint8_t *p, size_t avail)
{
    size_t n;
    uint8_t lo = 0x80, hi = 0xbf; // range of the second byte

    if (p[0] >= 0xc2 && p[0] <= 0xdf) {
        n = 2;
    } else if (p[0] >= 0xe0 && p[0] <= 0xef) {
        n = 3;
        lo = p[0] == 0xe0 ? 0xa0 : 0x80;
        hi = p[0] == 0xed ? 0x9f : 0xbf;
    } else if (p[0] >= 0xf0 && p[0] <= 0xf4) {
        n = 4;
        lo = p[0] == 0xf0 ? 0x90 : 0x80;
        hi = p[0] == 0xf4 ? 0x8f : 0xbf;
    } else {
        return 0;
    }
    if (n > avail || p[1] < lo || p[1] > hi) {
        return 0;
    }
    for (size_t i = 2; i < n; i++) {
        if ((p[i] & 0xc0) != 0x80) {
            return 0;
        }
    }
    return n;
}

/** A JSON string: quote, backslash and control bytes escaped, valid UTF-8 as is, anything else as U+FFFD. */
static void str(writer_t *w, const char *text)
{
    static const char hex[] = "0123456789abcdef";
    const uint8_t *p = (const uint8_t *)text;
    const size_t total = strnlen(text, MESH_SCAN_NAME_LEN);
    size_t i = 0;

    raw(w, "\"");
    while (i < total) {
        const uint8_t c = p[i];

        if (c == '"' || c == '\\') {
            const char esc[2] = { '\\', (char)c };

            put(w, esc, 2);
        } else if (c < 0x20) {
            const char esc[6] = { '\\', 'u', '0', '0', hex[c >> 4], hex[c & 15] };

            put(w, esc, 6);
        } else if (c < 0x80) {
            put(w, (const char *)&p[i], 1);
        } else {
            const size_t n = utf8_len(&p[i], total - i);

            if (n == 0) {
                raw(w, "\xef\xbf\xbd");
                i++;
                continue;
            }
            put(w, (const char *)&p[i], n);
            i += n;
            continue;
        }
        i++;
    }
    raw(w, "\"");
}

static void ext_address(writer_t *w, const uint8_t ext[8])
{
    static const char hex[] = "0123456789abcdef";
    char out[18] = { '"' };

    for (size_t i = 0; i < 8; i++) {
        out[1 + 2 * i] = hex[ext[i] >> 4];
        out[2 + 2 * i] = hex[ext[i] & 15];
    }
    out[17] = '"';
    put(w, out, sizeof(out));
}

/** `addr` in RFC 5952 form: lower case, no leading zeros, the first longest run of two or more zero groups as "::". */
static void ip6_text(writer_t *w, const uint8_t addr[16])
{
    static const char hex[] = "0123456789abcdef";
    uint16_t group[8];
    int best = -1, best_len = 0;

    for (int i = 0; i < 8; i++) {
        group[i] = (uint16_t)(addr[2 * i] << 8 | addr[2 * i + 1]);
    }
    for (int i = 0; i < 8;) {
        int j = i;

        while (j < 8 && group[j] == 0) {
            j++;
        }
        if (j - i > best_len && j - i >= 2) {
            best = i;
            best_len = j - i;
        }
        i = j > i ? j : i + 1;
    }
    for (int i = 0; i < 8; i++) {
        if (i == best) {
            raw(w, "::");
            i += best_len - 1;
            continue;
        }
        if (i > 0 && i != best + best_len) {
            raw(w, ":");
        }
        char digits[4];
        size_t n = 0;

        for (int shift = 12; shift >= 0; shift -= 4) {
            const uint8_t d = (group[i] >> shift) & 15;

            if (n > 0 || d > 0 || shift == 0) {
                digits[n++] = hex[d];
            }
        }
        put(w, digits, n);
    }
}

static void ip6_string(writer_t *w, const uint8_t addr[16])
{
    raw(w, "\"");
    ip6_text(w, addr);
    raw(w, "\"");
}

static void prefix_string(writer_t *w, const mesh_scan_prefix_t prefix)
{
    uint8_t full[16] = { 0 };

    memcpy(full, prefix, sizeof(mesh_scan_prefix_t));
    raw(w, "\"");
    ip6_text(w, full);
    raw(w, "/64\"");
}

static void addr_list(writer_t *w, const mesh_scan_ip6_t *addrs, size_t n)
{
    raw(w, "[");
    for (size_t i = 0; i < n; i++) {
        comma(w, i);
        ip6_string(w, addrs[i]);
    }
    raw(w, "]");
}

/** A raw 0..0xffff error rate as percent with one decimal, rounded to nearest. */
static void percent(writer_t *w, uint16_t value)
{
    const uint32_t tenths = ((uint32_t)value * 1000 + 32767) / 65535;

    u64(w, tenths / 10);
    raw(w, ".");
    u64(w, tenths % 10);
}

static void key_u64(writer_t *w, const char *key, uint64_t value)
{
    raw(w, key);
    u64(w, value);
}

static void write_child(writer_t *w, const mesh_scan_router_t *r, const mesh_scan_child_t *c)
{
    // The child-table fields are known only when the router answered the query and the table listed this child.
    const bool detail = r->children_detail && c->has_ext;

    key_u64(w, "{\"rloc16\":", c->rloc16);
    if (detail) {
        raw(w, ",\"ext_address\":");
        ext_address(w, c->ext);
    }
    raw(w, ",\"mode\":{\"rx_on_when_idle\":");
    boolean(w, c->rx_on_when_idle);
    raw(w, ",\"full_thread_device\":");
    boolean(w, c->full_thread_device);
    raw(w, ",\"full_network_data\":");
    boolean(w, c->full_network_data);
    key_u64(w, "},\"lq_in\":", c->lq_in);
    if (detail) {
        key_u64(w, ",\"timeout_s\":", c->timeout_s);
        key_u64(w, ",\"age_s\":", c->age_s);
        key_u64(w, ",\"connected_s\":", c->connected_s);
        raw(w, ",\"rssi_avg\":");
        i64(w, c->rssi_avg);
        raw(w, ",\"rssi_last\":");
        i64(w, c->rssi_last);
        key_u64(w, ",\"link_margin_db\":", c->link_margin_db);
        if (c->has_err_rate) {
            raw(w, ",\"frame_error_pct\":");
            percent(w, c->frame_err);
            raw(w, ",\"message_error_pct\":");
            percent(w, c->msg_err);
        }
        key_u64(w, ",\"version\":", c->version);
        raw(w, ",\"csl\":");
        boolean(w, c->csl);
    }
    raw(w, ",\"addresses\":");
    addr_list(w, c->addrs, c->n_addrs);
    raw(w, "}");
}

static void write_router(writer_t *w, const mesh_scan_router_t *r)
{
    if (!r->responded) {
        key_u64(w, "{\"router_id\":", r->router_id);
        key_u64(w, ",\"rloc16\":", r->rloc16);
        raw(w, ",\"responded\":false}");
        return;
    }
    key_u64(w, "{\"rloc16\":", r->rloc16);
    key_u64(w, ",\"router_id\":", r->router_id);
    raw(w, ",\"ext_address\":");
    ext_address(w, r->ext);
    if (r->has_version) {
        key_u64(w, ",\"version\":", r->version);
    }
    raw(w, ",\"leader\":");
    boolean(w, r->leader);
    raw(w, ",\"this_device\":");
    boolean(w, r->this_device);
    raw(w, ",\"border_router\":");
    boolean(w, r->border_router);
    raw(w, ",\"links\":[");
    size_t links = 0;

    for (size_t id = 0; id < MESH_SCAN_MAX_ROUTERS; id++) {
        if (r->lq_in[id] > 0) {
            comma(w, links++);
            key_u64(w, "{\"router_id\":", id);
            key_u64(w, ",\"lq_in\":", r->lq_in[id]);
            raw(w, "}");
        }
    }
    raw(w, "],\"addresses\":");
    addr_list(w, r->addrs, r->n_addrs);
    raw(w, ",\"responded\":true,\"children_detail\":");
    boolean(w, r->children_detail);
    raw(w, ",\"children\":[");
    for (size_t i = 0; i < r->n_children; i++) {
        comma(w, i);
        write_child(w, r, &r->children[i]);
    }
    raw(w, "]}");
}

static void write_network(writer_t *w, const mesh_scan_network_t *n)
{
    raw(w, "{\"name\":");
    str(w, n->name);
    key_u64(w, ",\"channel\":", n->channel);
    key_u64(w, ",\"partition_id\":", n->partition_id);
    key_u64(w, ",\"leader_router_id\":", n->leader_router_id);
    raw(w, ",\"mesh_local_prefix\":");
    prefix_string(w, n->ml_prefix);
    key_u64(w, ",\"this_rloc16\":", n->this_rloc16);
    raw(w, ",\"omr_prefixes\":[");
    for (size_t i = 0; i < n->n_omr; i++) {
        comma(w, i);
        prefix_string(w, n->omr[i]);
    }
    raw(w, "]}");
}

static void write_srp_host(writer_t *w, const mesh_scan_srp_host_t *h)
{
    raw(w, "{\"name\":");
    str(w, h->name);
    raw(w, ",\"addresses\":");
    addr_list(w, h->addrs, h->n_addrs);
    raw(w, ",\"services\":[");
    for (size_t i = 0; i < h->n_services; i++) {
        comma(w, i);
        raw(w, "{\"instance\":");
        str(w, h->services[i].instance);
        raw(w, ",\"type\":");
        str(w, h->services[i].type);
        raw(w, "}");
    }
    raw(w, "]}");
}

static void write_result(writer_t *w, const mesh_scan_result_t *r)
{
    raw(w, "{\"partial\":");
    boolean(w, r->partial);
    raw(w, ",\"network\":");
    write_network(w, &r->network);
    raw(w, ",\"routers\":[");
    size_t routers = 0;

    for (size_t id = 0; id < MESH_SCAN_MAX_ROUTERS; id++) {
        if (r->routers[id].present) {
            comma(w, routers++);
            write_router(w, &r->routers[id]);
        }
    }
    raw(w, "],\"srp_hosts\":[");
    for (size_t i = 0; i < r->n_srp; i++) {
        comma(w, i);
        write_srp_host(w, &r->srp[i]);
    }
    raw(w, "]}");
}

static const char *error_name(mesh_scan_error_t error)
{
    switch (error) {
    case MESH_SCAN_ERR_BUSY:
        return "busy";
    case MESH_SCAN_ERR_NO_MEMORY:
        return "no_memory";
    case MESH_SCAN_ERR_STOPPED:
    case MESH_SCAN_ERR_NONE:
    default:
        return "stopped";
    }
}

esp_err_t mesh_scan_render_view(const mesh_scan_view_t *view, mesh_scan_sink_t sink, void *ctx)
{
    ESP_RETURN_ON_FALSE(view && sink, ESP_ERR_INVALID_ARG, TAG, "no view or sink");
    ESP_RETURN_ON_FALSE(view->state != MESH_SCAN_DONE || view->result, ESP_ERR_INVALID_ARG, TAG, "done without result");

    writer_t w = { .sink = sink, .ctx = ctx, .err = ESP_OK };

    switch (view->state) {
    case MESH_SCAN_RUNNING:
        raw(&w, "{\"state\":\"running\",\"progress\":{\"routers\":");
        u64(&w, view->routers);
        key_u64(&w, ",\"queried\":", view->queried);
        raw(&w, "}}");
        break;
    case MESH_SCAN_DONE:
        key_u64(&w, "{\"state\":\"done\",\"age_s\":", view->age_s);
        raw(&w, ",\"result\":");
        write_result(&w, view->result);
        raw(&w, "}");
        break;
    case MESH_SCAN_FAILED:
        raw(&w, "{\"state\":\"failed\",\"error\":\"");
        raw(&w, error_name(view->error));
        key_u64(&w, "\",\"age_s\":", view->age_s);
        raw(&w, "}");
        break;
    case MESH_SCAN_IDLE:
    default:
        raw(&w, "{\"state\":\"idle\"}");
        break;
    }
    flush_buffer(&w);
    ESP_RETURN_ON_ERROR(w.err, TAG, "sink failed");
    return ESP_OK;
}
