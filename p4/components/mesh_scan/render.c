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

/**
 * A JSON string of at most `max` bytes of `text`: quote, backslash and control bytes escaped, valid UTF-8 as is,
 * anything else as U+FFFD.
 */
static void str_max(writer_t *w, const char *text, size_t max)
{
    static const char hex[] = "0123456789abcdef";
    const uint8_t *p = (const uint8_t *)text;
    const size_t total = strnlen(text, max);
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

static void str(writer_t *w, const char *text)
{
    str_max(w, text, MESH_SCAN_NAME_LEN);
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

static const char *info_state_name(mesh_scan_info_state_t state)
{
    switch (state) {
    case MESH_SCAN_INFO_ANSWERED:
        return "answered";
    case MESH_SCAN_INFO_NO_ANSWER:
        return "no_answer";
    case MESH_SCAN_INFO_CUT_SHORT:
        return "cut_short";
    case MESH_SCAN_INFO_NOT_ASKED:
    default:
        return "not_asked";
    }
}

/** ,"key":"<text>" unless `text` (an array of `max` bytes) is empty. */
static void info_string(writer_t *w, const char *key, const char *text, size_t max)
{
    if (strnlen(text, max) == 0) {
        return;
    }
    raw(w, key);
    str_max(w, text, max);
}

/** ,"info":{...}: the fields only after an answer, and only the non-empty ones. */
static void write_info(writer_t *w, const mesh_scan_info_t *info)
{
    raw(w, ",\"info\":{\"state\":\"");
    raw(w, info_state_name(info->state));
    raw(w, "\"");
    if (info->state == MESH_SCAN_INFO_ANSWERED) {
        if (info->has_eui64) {
            raw(w, ",\"eui64\":");
            ext_address(w, info->eui64);
        }
        info_string(w, ",\"vendor_name\":", info->vendor_name, sizeof(info->vendor_name));
        info_string(w, ",\"vendor_model\":", info->vendor_model, sizeof(info->vendor_model));
        info_string(w, ",\"vendor_sw_version\":", info->vendor_sw_version, sizeof(info->vendor_sw_version));
        info_string(w, ",\"thread_stack_version\":", info->thread_stack_version, sizeof(info->thread_stack_version));
        info_string(w, ",\"vendor_app_url\":", info->vendor_app_url, sizeof(info->vendor_app_url));
        if (info->has_oui) {
            static const char hex[] = "0123456789abcdef";
            char digits[6];

            for (size_t i = 0; i < sizeof(digits); i++) {
                digits[i] = hex[(info->oui >> (20 - 4 * i)) & 15];
            }
            raw(w, ",\"vendor_oui\":\"");
            put(w, digits, sizeof(digits));
            raw(w, "\"");
        }
    }
    raw(w, "}");
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
    if (!c->addresses_reported) {
        raw(w, ",\"addresses_reported\":false"); // asked directly and gave no answer (spec 3.4)
    }
    write_info(w, &c->info);
    raw(w, "}");
}

static void pref_or_null(writer_t *w, int16_t pref)
{
    if (pref == MESH_SCAN_PREF_NONE) {
        raw(w, "null");
    } else {
        u64(w, (uint64_t)pref);
    }
}

// Only this board knows the radios of its own links; other routers' links carry no transport.
static void write_transport(writer_t *w, const mesh_scan_result_t *res, const mesh_scan_router_t *r, size_t id)
{
    if (!r->this_device || !res->has_transport || !res->this_links[id].valid) {
        return;
    }
    raw(w, ",\"transport\":{\"ieee802154\":");
    pref_or_null(w, res->this_links[id].pref_154);
    raw(w, ",\"trel\":");
    pref_or_null(w, res->this_links[id].pref_trel);
    raw(w, "}");
}

/*
 * A router's incoming link quality from router `id`, 0 for no link. This board's own links also come from its local
 * snapshot, so a link its diagnostic answer missed (it was taken earlier) still carries its transport.
 */
static uint8_t link_lq_in(const mesh_scan_result_t *res, const mesh_scan_router_t *r, size_t id)
{
    if (r->lq_in[id] > 0 || !r->this_device || !res->has_transport || !res->this_links[id].valid) {
        return r->lq_in[id];
    }
    return res->this_links[id].lq_in;
}

static void write_router(writer_t *w, const mesh_scan_result_t *res, const mesh_scan_router_t *r)
{
    if (!r->responded) {
        key_u64(w, "{\"router_id\":", r->router_id);
        key_u64(w, ",\"rloc16\":", r->rloc16);
        raw(w, ",\"responded\":false");
        write_info(w, &r->info);
        raw(w, "}");
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
        const uint8_t lq_in = link_lq_in(res, r, id);

        if (lq_in > 0) {
            comma(w, links++);
            key_u64(w, "{\"router_id\":", id);
            key_u64(w, ",\"lq_in\":", lq_in);
            write_transport(w, res, r, id);
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
    raw(w, "]");
    write_info(w, &r->info);
    raw(w, "}");
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

/** `data` as a JSON string in standard base64 with padding. */
static void base64_string(writer_t *w, const uint8_t *data, size_t len)
{
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    raw(w, "\"");
    for (size_t i = 0; i < len; i += 3) {
        const size_t left = len - i;
        const uint32_t bits = (uint32_t)data[i] << 16 | (left > 1 ? (uint32_t)data[i + 1] << 8 : 0) |
                              (left > 2 ? data[i + 2] : 0);
        const char quad[4] = {
            alphabet[bits >> 18 & 63],
            alphabet[bits >> 12 & 63],
            left > 1 ? alphabet[bits >> 6 & 63] : '=',
            left > 2 ? alphabet[bits & 63] : '=',
        };

        put(w, quad, sizeof(quad));
    }
    raw(w, "\"");
}

static const char *srp_state_name(mesh_scan_srp_state_t state)
{
    switch (state) {
    case MESH_SCAN_SRP_ANSWERED:
        return "answered";
    case MESH_SCAN_SRP_CUT_SHORT:
        return "cut_short";
    case MESH_SCAN_SRP_NO_ANSWER:
    default:
        return "no_answer";
    }
}

static void write_srp_service(writer_t *w, const mesh_scan_result_t *r, const mesh_scan_srp_host_t *h, size_t index)
{
    const __typeof__(h->services[0]) *s = &h->services[index];
    // Offsets come from the collector; a bad one renders as no data instead of reading outside the pool.
    const bool txt_ok = s->has_txt && s->txt_len > 0 && r->pool && s->txt_off + (size_t)s->txt_len <= r->pool_used;
    const uint8_t *labels = r->pool ? r->pool + s->sub_off : NULL;
    size_t left = r->pool && s->sub_off <= r->pool_used ? r->pool_used - s->sub_off : 0;

    raw(w, "{\"instance\":");
    str(w, s->instance);
    raw(w, ",\"type\":");
    str(w, s->type);
    key_u64(w, ",\"port\":", s->port);
    key_u64(w, ",\"source\":", s->source);
    raw(w, ",\"subtypes\":[");
    for (size_t i = 0; i < s->n_sub && left > 0; i++) {
        const size_t n = strnlen((const char *)labels, left);

        if (n == left) {
            break; // no NUL inside the used pool: not a label
        }
        comma(w, i);
        str(w, (const char *)labels);
        labels += n + 1;
        left -= n + 1;
    }
    raw(w, "],\"txt\":");
    if (txt_ok) {
        base64_string(w, r->pool + s->txt_off, s->txt_len);
    } else {
        raw(w, "\"\"");
    }
    raw(w, ",\"txt_truncated\":");
    boolean(w, s->txt_truncated);
    raw(w, "}");
}

static void write_srp_host(writer_t *w, const mesh_scan_result_t *r, const mesh_scan_srp_host_t *h)
{
    raw(w, "{\"name\":");
    str(w, h->name);
    raw(w, ",\"addresses\":");
    addr_list(w, h->addrs, h->n_addrs);
    raw(w, ",\"services\":[");
    for (size_t i = 0; i < h->n_services; i++) {
        comma(w, i);
        write_srp_service(w, r, h, i);
    }
    key_u64(w, "],\"source\":", h->source);
    if (h->local) {
        key_u64(w, ",\"lease_s\":", h->lease_s);
        key_u64(w, ",\"key_lease_s\":", h->key_lease_s);
    } else {
        key_u64(w, ",\"ttl_s\":", h->ttl_s);
    }
    raw(w, "}");
}

static void write_srp_server(writer_t *w, const mesh_scan_srp_server_t *s)
{
    key_u64(w, "{\"rloc16\":", s->rloc16);
    raw(w, ",\"address\":");
    ip6_string(w, s->addr);
    key_u64(w, ",\"port\":", s->port);
    raw(w, ",\"this_board\":");
    boolean(w, s->this_board);
    raw(w, ",\"state\":\"");
    raw(w, srp_state_name(s->state));
    key_u64(w, "\",\"hosts\":", s->hosts);
    raw(w, "}");
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
            write_router(w, r, &r->routers[id]);
        }
    }
    raw(w, "],\"srp_hosts\":[");
    for (size_t i = 0; i < r->n_srp; i++) {
        comma(w, i);
        write_srp_host(w, r, &r->srp[i]);
    }
    raw(w, "],\"srp_servers\":[");
    for (size_t i = 0; i < r->n_srp_servers; i++) {
        comma(w, i);
        write_srp_server(w, &r->srp_servers[i]);
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
        key_u64(&w, ",\"children\":{\"total\":", view->children_total);
        key_u64(&w, ",\"queried\":", view->children_queried);
        key_u64(&w, "},\"info\":{\"total\":", view->info_total);
        key_u64(&w, ",\"answered\":", view->info_answered);
        key_u64(&w, ",\"sleepy_waiting\":", view->sleepy_waiting);
        key_u64(&w, ",\"wait_s\":", view->sleepy_wait_s);
        raw(&w, "}}}");
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
