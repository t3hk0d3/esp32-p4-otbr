// Routing settings (spec section 6): defaults, the JSON form with its validation and the change set are pure and
// host-tested; the NVS copy is target only.
#include "settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "ipv6_text.h"

#ifndef CONFIG_IDF_TARGET_LINUX
#include "esp_check.h"
#include "esp_log.h"
#include "nvs.h"
#endif

#define FIELD_BODY "body"
#define FIELD_METHOD "method"
#define FIELD_OMR "omr"
#define FIELD_OMR_MODE "omr.mode"
#define FIELD_OMR_PREFIX "omr.prefix"
#define FIELD_ANNOUNCE_ULA "announce_ula"
#define FIELD_BGP "bgp"
#define FIELD_LOCAL_ASN "bgp.local_asn"
#define FIELD_ROUTER_ID "bgp.router_id"
#define FIELD_HOLD_TIME "bgp.hold_time"
#define FIELD_PEERS "bgp.peers"

#define AS_TRANS 23456
#define ASN_MAX 4294967295.0
#define HOLD_TIME_MIN 3 /* besides 0 */
#define HOLD_TIME_MAX 65535
#define IPV6_GROUPS 8
#define PREFIX_TEXT_MAX 44         /* ROUTING_IPV6_TEXT_MAX + "/64" */
#define PREFIX_SUFFIX "/64"
#define IPV4_TEXT_MAX 16
#define OCTETS 4
#define OCTET_MAX 255
#define BITS_PER_OCTET 8
#define MS_PER_S 1000

static const char *const METHOD_NAMES[] = {
    [ROUTING_METHOD_PD] = "pd",
    [ROUTING_METHOD_RIPNG] = "ripng",
    [ROUTING_METHOD_BGP] = "bgp",
    [ROUTING_METHOD_OFF] = "off",
};

static const char *const OMR_MODE_NAMES[] = {
    [ROUTING_OMR_AUTO] = "auto",
    [ROUTING_OMR_CUSTOM] = "custom",
};

static const char *const BGP_STATE_NAMES[] = {
    [ROUTING_BGP_IDLE] = "idle",
    [ROUTING_BGP_CONNECT] = "connect",
    [ROUTING_BGP_ACTIVE] = "active",
    [ROUTING_BGP_OPENSENT] = "opensent",
    [ROUTING_BGP_OPENCONFIRM] = "openconfirm",
    [ROUTING_BGP_ESTABLISHED] = "established",
};

#define COUNT_OF(a) (sizeof(a) / sizeof((a)[0]))

/* ---- address text ---- */

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/* Strict IPv6 text: hex groups of 1..4 digits, at most one "::", no zone, no embedded IPv4, nothing after it. */
static bool parse_ipv6(const char *text, uint8_t out[16])
{
    uint16_t groups[IPV6_GROUPS] = {0};
    int count = 0;
    int gap = -1; /* index where "::" stands, -1 none */
    const char *p = text;

    if (p[0] == ':' && p[1] == ':') {
        gap = 0;
        p += 2;
    }
    while (*p != '\0') {
        uint32_t value = 0;
        int digits = 0;

        for (int d; digits < 4 && (d = hex_value(*p)) >= 0; digits++, p++) {
            value = (value << 4) | (uint32_t)d;
        }
        if (digits == 0 || count == IPV6_GROUPS) {
            return false;
        }
        groups[count++] = (uint16_t)value;
        if (*p == '\0') {
            break;
        }
        if (*p != ':') {
            return false;
        }
        p++;
        if (*p == ':') {
            if (gap >= 0) {
                return false;
            }
            gap = count;
            p++;
        } else if (*p == '\0') {
            return false; /* trailing single colon */
        }
    }
    if (gap < 0 ? count != IPV6_GROUPS : count > IPV6_GROUPS - 1) {
        return false;
    }
    const int zeros = IPV6_GROUPS - count;

    memset(out, 0, 16);
    for (int i = 0, g = 0; i < count; i++, g++) {
        if (i == gap) {
            g += zeros;
        }
        out[2 * g] = (uint8_t)(groups[i] >> 8);
        out[2 * g + 1] = (uint8_t)groups[i];
    }
    return true;
}

void routing_ipv6_to_text(const uint8_t addr[16], char *buf, size_t cap)
{
    uint16_t groups[IPV6_GROUPS];
    int best = -1, best_len = 1;
    size_t len = 0;

    for (int i = 0; i < IPV6_GROUPS; i++) {
        groups[i] = (uint16_t)((addr[2 * i] << 8) | addr[2 * i + 1]);
    }
    for (int i = 0; i < IPV6_GROUPS;) {
        int run = 0;

        while (i + run < IPV6_GROUPS && groups[i + run] == 0) {
            run++;
        }
        if (run > best_len) {
            best = i;
            best_len = run;
        }
        i += run > 0 ? run : 1;
    }
    buf[0] = '\0';
    for (int i = 0; i < IPV6_GROUPS && len < cap; i++) {
        if (i == best) {
            len += (size_t)snprintf(buf + len, cap - len, "::");
            i += best_len - 1;
            continue;
        }
        const bool after_gap = best >= 0 && i == best + best_len;

        len += (size_t)snprintf(buf + len, cap - len, "%s%x", i == 0 || after_gap ? "" : ":", groups[i]);
    }
}

static bool parse_prefix64(const char *text, routing_prefix64_t *out)
{
    const char *slash = strchr(text, '/');
    char addr_text[ROUTING_IPV6_TEXT_MAX];
    uint8_t addr[16];

    if (!slash || strcmp(slash, PREFIX_SUFFIX) != 0 || (size_t)(slash - text) >= sizeof(addr_text)) {
        return false;
    }
    memcpy(addr_text, text, (size_t)(slash - text));
    addr_text[slash - text] = '\0';
    if (!parse_ipv6(addr_text, addr)) {
        return false;
    }
    for (int i = 8; i < 16; i++) {
        if (addr[i] != 0) {
            return false; /* host bits set: an address, not a /64 */
        }
    }
    memcpy(out->b, addr, sizeof(out->b));
    return true;
}

static void format_prefix64(const routing_prefix64_t *p, char *buf, size_t cap)
{
    uint8_t addr[16] = {0};

    memcpy(addr, p->b, sizeof(p->b));
    routing_ipv6_to_text(addr, buf, cap);
    strncat(buf, PREFIX_SUFFIX, cap - strlen(buf) - 1);
}

/* Strict dotted quad: four decimal fields 0..255, no sign, no spaces, no leading zeros. Host order. */
static bool parse_ipv4(const char *text, uint32_t *out)
{
    uint32_t value = 0;

    for (int octet = 0; octet < OCTETS; octet++) {
        if (*text < '0' || *text > '9') {
            return false;
        }
        const char *start = text;
        uint32_t n = 0;

        while (*text >= '0' && *text <= '9' && text - start < 4) {
            n = n * 10 + (uint32_t)(*text - '0');
            text++;
        }
        if (n > OCTET_MAX || (text - start > 1 && *start == '0')) {
            return false;
        }
        value = (value << BITS_PER_OCTET) | n;
        if (octet < OCTETS - 1) {
            if (*text != '.') {
                return false;
            }
            text++;
        }
    }
    *out = value;
    return *text == '\0';
}

static void format_ipv4(uint32_t v, char *buf, size_t cap)
{
    snprintf(buf, cap, "%u.%u.%u.%u", (unsigned)(v >> 24), (unsigned)((v >> 16) & OCTET_MAX),
             (unsigned)((v >> 8) & OCTET_MAX), (unsigned)(v & OCTET_MAX));
}

static bool is_link_local(const uint8_t *a)
{
    return a[0] == 0xfe && (a[1] & 0xc0) == 0x80; /* fe80::/10 */
}

static bool is_multicast(const uint8_t *a)
{
    return a[0] == 0xff; /* ff00::/8 */
}

static bool is_unspecified(const uint8_t a[16])
{
    static const uint8_t zero[16] = {0};

    return memcmp(a, zero, 16) == 0;
}

/* ---- defaults, diff ---- */

void routing_settings_defaults(routing_settings_t *out)
{
    memset(out, 0, sizeof(*out));
    out->method = ROUTING_METHOD_PD;
    out->omr_mode = ROUTING_OMR_AUTO;
    out->local_asn = ROUTING_DEFAULT_LOCAL_ASN;
    out->hold_time = ROUTING_DEFAULT_HOLD_TIME;
}

static bool peers_equal(const routing_settings_t *a, const routing_settings_t *b)
{
    if (a->peer_count != b->peer_count) {
        return false;
    }
    for (int i = 0; i < a->peer_count && i < ROUTING_MAX_PEERS; i++) {
        if (memcmp(a->peers[i].addr, b->peers[i].addr, 16) != 0 || a->peers[i].asn != b->peers[i].asn) {
            return false;
        }
    }
    return true;
}

uint32_t routing_settings_diff(const routing_settings_t *a, const routing_settings_t *b)
{
    uint32_t changes = 0;

    if (a->method != b->method) {
        changes |= ROUTING_CHANGE_METHOD;
    }
    if (a->omr_mode != b->omr_mode || memcmp(&a->omr_prefix, &b->omr_prefix, sizeof(a->omr_prefix)) != 0 ||
            a->announce_ula != b->announce_ula) {
        changes |= ROUTING_CHANGE_PREFIXES;
    }
    if (a->local_asn != b->local_asn || a->router_id_set != b->router_id_set || a->router_id != b->router_id ||
            a->hold_time != b->hold_time || !peers_equal(a, b)) {
        changes |= ROUTING_CHANGE_BGP_SESSIONS;
    }
    return changes;
}

/* ---- JSON in ---- */

static esp_err_t fail(const char **bad_field, const char *field)
{
    if (bad_field) {
        *bad_field = field;
    }
    return ESP_ERR_INVALID_ARG;
}

/* A JSON integer in min..max. */
static bool get_integer(const cJSON *item, double min, double max, uint32_t *out)
{
    if (!cJSON_IsNumber(item) || item->valuedouble < min || item->valuedouble > max ||
            item->valuedouble != (double)(uint32_t)item->valuedouble) {
        return false;
    }
    *out = (uint32_t)item->valuedouble;
    return true;
}

static bool get_asn(const cJSON *item, uint32_t *out)
{
    return get_integer(item, 1, ASN_MAX, out) && *out != AS_TRANS;
}

static bool get_name(const cJSON *item, const char *const *names, size_t count, int *out)
{
    if (!cJSON_IsString(item)) {
        return false;
    }
    for (size_t i = 0; i < count; i++) {
        if (strcmp(item->valuestring, names[i]) == 0) {
            *out = (int)i;
            return true;
        }
    }
    return false;
}

static esp_err_t parse_omr(const cJSON *omr, const routing_validate_ctx_t *ctx, routing_settings_t *s,
                           const char **bad_field)
{
    int mode;

    if (!cJSON_IsObject(omr)) {
        return fail(bad_field, FIELD_OMR);
    }
    if (!get_name(cJSON_GetObjectItemCaseSensitive(omr, "mode"), OMR_MODE_NAMES, COUNT_OF(OMR_MODE_NAMES), &mode)) {
        return fail(bad_field, FIELD_OMR_MODE);
    }
    s->omr_mode = (routing_omr_mode_t)mode;
    if (s->omr_mode == ROUTING_OMR_AUTO) {
        return ESP_OK; /* the prefix is ignored */
    }
    const cJSON *prefix = cJSON_GetObjectItemCaseSensitive(omr, "prefix");

    if (!cJSON_IsString(prefix) || !parse_prefix64(prefix->valuestring, &s->omr_prefix) ||
            is_link_local(s->omr_prefix.b) || is_multicast(s->omr_prefix.b)) {
        return fail(bad_field, FIELD_OMR_PREFIX);
    }
    for (int i = 0; ctx && i < ctx->onlink_count && i < ROUTING_MAX_ONLINK; i++) {
        if (memcmp(&ctx->onlink[i], &s->omr_prefix, sizeof(s->omr_prefix)) == 0) {
            return fail(bad_field, FIELD_OMR_PREFIX); /* the same /64 would be on two links */
        }
    }
    return ESP_OK;
}

static bool is_own_address(const routing_validate_ctx_t *ctx, const uint8_t addr[16])
{
    for (int i = 0; ctx && i < ctx->own_count && i < ROUTING_MAX_OWN_ADDRS; i++) {
        if (memcmp(ctx->own_addrs[i], addr, 16) == 0) {
            return true;
        }
    }
    return false;
}

/* An address a peer may have: not multicast, not unspecified, not one of the board's own. */
static bool peer_address_ok(const routing_validate_ctx_t *ctx, const uint8_t addr[16])
{
    return !is_multicast(addr) && !is_unspecified(addr) && !is_own_address(ctx, addr);
}

static bool parse_peer(const cJSON *item, const routing_validate_ctx_t *ctx, const routing_settings_t *s,
                       routing_bgp_peer_t *out)
{
    const cJSON *address = cJSON_GetObjectItemCaseSensitive(item, "address");

    if (!cJSON_IsObject(item) || !cJSON_IsString(address) || !parse_ipv6(address->valuestring, out->addr) ||
            !peer_address_ok(ctx, out->addr) || !get_asn(cJSON_GetObjectItemCaseSensitive(item, "asn"), &out->asn)) {
        return false;
    }
    for (int i = 0; i < s->peer_count; i++) {
        if (memcmp(s->peers[i].addr, out->addr, 16) == 0) {
            return false; /* duplicate */
        }
    }
    return true;
}

static esp_err_t parse_bgp(const cJSON *bgp, const routing_validate_ctx_t *ctx, routing_settings_t *s,
                           const char **bad_field)
{
    uint32_t value;

    if (!cJSON_IsObject(bgp)) {
        return fail(bad_field, FIELD_BGP);
    }
    if (!get_asn(cJSON_GetObjectItemCaseSensitive(bgp, "local_asn"), &s->local_asn)) {
        return fail(bad_field, FIELD_LOCAL_ASN);
    }
    const cJSON *router_id = cJSON_GetObjectItemCaseSensitive(bgp, "router_id");

    s->router_id_set = false;
    s->router_id = 0;
    if (router_id && !cJSON_IsNull(router_id)) {
        if (!cJSON_IsString(router_id) || !parse_ipv4(router_id->valuestring, &value) || value == 0) {
            return fail(bad_field, FIELD_ROUTER_ID);
        }
        s->router_id_set = true;
        s->router_id = value;
    }
    if (!get_integer(cJSON_GetObjectItemCaseSensitive(bgp, "hold_time"), 0, HOLD_TIME_MAX, &value) ||
            (value != 0 && value < HOLD_TIME_MIN)) {
        return fail(bad_field, FIELD_HOLD_TIME);
    }
    s->hold_time = (uint16_t)value;
    const cJSON *peers = cJSON_GetObjectItemCaseSensitive(bgp, "peers");

    if (!cJSON_IsArray(peers) || cJSON_GetArraySize(peers) > ROUTING_MAX_PEERS) {
        return fail(bad_field, FIELD_PEERS);
    }
    s->peer_count = 0;
    memset(s->peers, 0, sizeof(s->peers));
    const cJSON *item;

    cJSON_ArrayForEach(item, peers) {
        if (!parse_peer(item, ctx, s, &s->peers[s->peer_count])) {
            return fail(bad_field, FIELD_PEERS);
        }
        s->peer_count++;
    }
    return ESP_OK;
}

static esp_err_t parse_settings(const cJSON *root, const routing_settings_t *current,
                                const routing_validate_ctx_t *ctx, routing_settings_t *s, const char **bad_field)
{
    int method;

    routing_settings_defaults(s);
    if (current) {
        s->local_asn = current->local_asn;
        s->router_id_set = current->router_id_set;
        s->router_id = current->router_id;
        s->hold_time = current->hold_time;
        s->peer_count = current->peer_count;
        memcpy(s->peers, current->peers, sizeof(s->peers));
    }
    if (!get_name(cJSON_GetObjectItemCaseSensitive(root, "method"), METHOD_NAMES, COUNT_OF(METHOD_NAMES), &method)) {
        return fail(bad_field, FIELD_METHOD);
    }
    s->method = (routing_method_t)method;
    esp_err_t err = parse_omr(cJSON_GetObjectItemCaseSensitive(root, "omr"), ctx, s, bad_field);

    if (err != ESP_OK) {
        return err;
    }
    const cJSON *announce_ula = cJSON_GetObjectItemCaseSensitive(root, "announce_ula");

    if (!cJSON_IsBool(announce_ula)) {
        return fail(bad_field, FIELD_ANNOUNCE_ULA);
    }
    s->announce_ula = cJSON_IsTrue(announce_ula);
    const cJSON *bgp = cJSON_GetObjectItemCaseSensitive(root, "bgp");

    if (bgp) { /* validated whatever the method, so that the stored BGP settings are always valid */
        err = parse_bgp(bgp, ctx, s, bad_field);
        if (err != ESP_OK) {
            return err;
        }
    } else {
        /* the peers kept from `current` get the same address checks: the board's own addresses may have changed */
        for (int i = 0; i < s->peer_count && i < ROUTING_MAX_PEERS; i++) {
            if (!peer_address_ok(ctx, s->peers[i].addr)) {
                return fail(bad_field, FIELD_PEERS);
            }
        }
    }
    if (s->method == ROUTING_METHOD_BGP && s->peer_count == 0) {
        return fail(bad_field, FIELD_PEERS);
    }
    return ESP_OK;
}

esp_err_t routing_settings_from_json(const char *json, const routing_settings_t *current,
                                     const routing_validate_ctx_t *ctx, routing_settings_t *out, const char **bad_field)
{
    routing_settings_t s;

    if (!json) {
        return fail(bad_field, FIELD_BODY);
    }
    cJSON *root = cJSON_ParseWithOpts(json, NULL, true); /* trailing text is an error */

    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return fail(bad_field, FIELD_BODY);
    }
    const esp_err_t err = parse_settings(root, current, ctx, &s, bad_field);

    cJSON_Delete(root);
    if (err == ESP_OK) {
        memcpy(out, &s, sizeof(s)); /* out may be current; memcpy keeps the zeroed padding */
    }
    return err;
}

bool routing_settings_from_stored(const char *text, routing_settings_t *out, const char **bad_field)
{
    routing_settings_defaults(out);
    if (!text) {
        return true;
    }
    return routing_settings_from_json(text, NULL, NULL, out, bad_field) == ESP_OK; /* out stays the defaults on error */
}

/* ---- JSON out ---- */

static bool add_prefix(cJSON *obj, const char *key, bool known, const routing_prefix64_t *p)
{
    char text[PREFIX_TEXT_MAX];

    if (!known) {
        return cJSON_AddNullToObject(obj, key) != NULL;
    }
    format_prefix64(p, text, sizeof(text));
    return cJSON_AddStringToObject(obj, key, text) != NULL;
}

static bool add_text_or_null(cJSON *obj, const char *key, const char *text)
{
    return (text ? cJSON_AddStringToObject(obj, key, text) : cJSON_AddNullToObject(obj, key)) != NULL;
}

static bool add_ipv4_or_null(cJSON *obj, const char *key, bool set, uint32_t v)
{
    char text[IPV4_TEXT_MAX];

    format_ipv4(v, text, sizeof(text));
    return add_text_or_null(obj, key, set ? text : NULL);
}

static bool add_address(cJSON *obj, const uint8_t addr[16])
{
    char text[ROUTING_IPV6_TEXT_MAX];

    routing_ipv6_to_text(addr, text, sizeof(text));
    return cJSON_AddStringToObject(obj, "address", text) != NULL;
}

static bool add_object(cJSON *parent, const char *key, cJSON *child)
{
    if (!child) {
        return false;
    }
    if (!cJSON_AddItemToObject(parent, key, child)) {
        cJSON_Delete(child);
        return false;
    }
    return true;
}

/* A new object in `array`, or NULL. */
static cJSON *add_array_object(cJSON *array)
{
    cJSON *obj = cJSON_CreateObject();

    if (obj && !cJSON_AddItemToArray(array, obj)) {
        cJSON_Delete(obj);
        return NULL;
    }
    return obj;
}

static cJSON *bgp_settings_to_cjson(const routing_settings_t *s)
{
    cJSON *bgp = cJSON_CreateObject();
    bool ok = bgp && cJSON_AddNumberToObject(bgp, "local_asn", s->local_asn) &&
              add_ipv4_or_null(bgp, "router_id", s->router_id_set, s->router_id) &&
              cJSON_AddNumberToObject(bgp, "hold_time", s->hold_time);
    cJSON *peers = ok ? cJSON_AddArrayToObject(bgp, "peers") : NULL;

    ok = peers != NULL;
    for (int i = 0; ok && i < s->peer_count && i < ROUTING_MAX_PEERS; i++) {
        cJSON *peer = add_array_object(peers);

        ok = peer && add_address(peer, s->peers[i].addr) && cJSON_AddNumberToObject(peer, "asn", s->peers[i].asn);
    }
    if (!ok) {
        cJSON_Delete(bgp);
        return NULL;
    }
    return bgp;
}

static cJSON *bgp_status_to_cjson(const routing_settings_t *s, const routing_status_t *st)
{
    cJSON *bgp = cJSON_CreateObject();
    bool ok = bgp && add_ipv4_or_null(bgp, "router_id", st->bgp_router_id != 0, st->bgp_router_id) &&
              add_text_or_null(bgp, "blocked", st->bgp_blocked);
    cJSON *peers = ok ? cJSON_AddArrayToObject(bgp, "peers") : NULL;

    ok = peers != NULL;
    for (int i = 0; ok && i < s->peer_count && i < ROUTING_MAX_PEERS; i++) {
        const routing_bgp_peer_status_t *p = &st->peers[i];
        cJSON *peer = add_array_object(peers);
        char last_error[ROUTING_LAST_ERROR_LEN];
        const bool state_ok = (unsigned)p->state < COUNT_OF(BGP_STATE_NAMES);

        memcpy(last_error, p->last_error, sizeof(last_error));
        last_error[sizeof(last_error) - 1] = '\0'; /* the task may have filled every byte */
        ok = peer && add_address(peer, s->peers[i].addr) &&
             cJSON_AddStringToObject(peer, "state", BGP_STATE_NAMES[state_ok ? p->state : ROUTING_BGP_IDLE]) &&
             cJSON_AddNumberToObject(peer, "uptime_s", p->uptime_s) &&
             add_text_or_null(peer, "last_error", last_error[0] ? last_error : NULL) &&
             cJSON_AddNumberToObject(peer, "received_dropped", p->received_dropped);
    }
    if (!ok) {
        cJSON_Delete(bgp);
        return NULL;
    }
    return bgp;
}

static cJSON *ripng_status_to_cjson(const routing_ripng_status_t *r)
{
    cJSON *ripng = cJSON_CreateObject();
    const bool ok = ripng &&
                    (r->last_update_ms_ago < 0 ? cJSON_AddNullToObject(ripng, "last_update_s_ago") :
                     cJSON_AddNumberToObject(ripng, "last_update_s_ago", (double)(r->last_update_ms_ago / MS_PER_S))) &&
                    cJSON_AddNumberToObject(ripng, "updates_sent", r->updates_sent) &&
                    cJSON_AddNumberToObject(ripng, "requests_answered", r->requests_answered) &&
                    cJSON_AddNumberToObject(ripng, "responses_dropped", r->responses_dropped);

    if (!ok) {
        cJSON_Delete(ripng);
        return NULL;
    }
    return ripng;
}

static cJSON *status_to_cjson(const routing_settings_t *s, const routing_status_t *st)
{
    cJSON *status = cJSON_CreateObject();
    bool ok = status && add_prefix(status, "omr_prefix", st->omr_prefix_known, &st->omr_prefix) &&
              add_prefix(status, "ula_prefix", st->ula_prefix_known, &st->ula_prefix);
    cJSON *announced = ok ? cJSON_AddArrayToObject(status, "announced") : NULL;

    ok = announced != NULL;
    for (int i = 0; ok && i < st->announced.count && i < ROUTING_MAX_ANNOUNCED; i++) {
        char text[PREFIX_TEXT_MAX];

        format_prefix64(&st->announced.p[i], text, sizeof(text));
        cJSON *item = cJSON_CreateString(text);

        ok = item && cJSON_AddItemToArray(announced, item);
        if (!ok) {
            cJSON_Delete(item);
        }
    }
    if (ok) {
        ok = s->method == ROUTING_METHOD_BGP ? add_object(status, "bgp", bgp_status_to_cjson(s, st)) :
             cJSON_AddNullToObject(status, "bgp") != NULL;
    }
    if (ok) {
        ok = s->method == ROUTING_METHOD_RIPNG ? add_object(status, "ripng", ripng_status_to_cjson(&st->ripng)) :
             cJSON_AddNullToObject(status, "ripng") != NULL;
    }
    if (!ok) {
        cJSON_Delete(status);
        return NULL;
    }
    return status;
}

static cJSON *omr_to_cjson(const routing_settings_t *s)
{
    cJSON *omr = cJSON_CreateObject();
    const bool ok = omr && cJSON_AddStringToObject(omr, "mode", OMR_MODE_NAMES[s->omr_mode]) &&
                    add_prefix(omr, "prefix", s->omr_mode == ROUTING_OMR_CUSTOM, &s->omr_prefix);

    if (!ok) {
        cJSON_Delete(omr);
        return NULL;
    }
    return omr;
}

char *routing_settings_to_json(const routing_settings_t *s, const routing_status_t *status)
{
    if ((unsigned)s->method >= COUNT_OF(METHOD_NAMES) || (unsigned)s->omr_mode >= COUNT_OF(OMR_MODE_NAMES)) {
        return NULL;
    }
    cJSON *root = cJSON_CreateObject();
    const bool ok = root && cJSON_AddStringToObject(root, "method", METHOD_NAMES[s->method]) &&
                    add_object(root, "omr", omr_to_cjson(s)) &&
                    cJSON_AddBoolToObject(root, "announce_ula", s->announce_ula) &&
                    add_object(root, "bgp", bgp_settings_to_cjson(s)) &&
                    (status ? add_object(root, "status", status_to_cjson(s, status)) :
                     cJSON_AddNullToObject(root, "status") != NULL);
    char *text = ok ? cJSON_PrintUnformatted(root) : NULL;

    cJSON_Delete(root);
    return text;
}

/* ---- NVS (target) ---- */

#ifndef CONFIG_IDF_TARGET_LINUX
static const char *TAG = "routing";

#define NVS_NAMESPACE "routing"
#define NVS_KEY "settings"

esp_err_t routing_settings_load(routing_settings_t *out)
{
    nvs_handle_t nvs;
    size_t len = 0;
    char *text = NULL;
    const char *bad = NULL;
    esp_err_t ret;

    routing_settings_defaults(out);
    ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK; /* the namespace is created by the first store */
    }
    ESP_RETURN_ON_ERROR(ret, TAG, "cannot open the %s storage", NVS_NAMESPACE);
    ret = nvs_get_str(nvs, NVS_KEY, NULL, &len);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        ret = ESP_OK; /* nothing stored: text stays NULL */
    } else {
        ESP_GOTO_ON_ERROR(ret, done, TAG, "reading the stored settings failed");
        text = malloc(len);
        ESP_GOTO_ON_FALSE(text, ESP_ERR_NO_MEM, done, TAG, "no memory for the stored settings");
        ESP_GOTO_ON_ERROR(nvs_get_str(nvs, NVS_KEY, text, &len), done, TAG, "reading the stored settings failed");
    }
    if (!routing_settings_from_stored(text, out, &bad)) {
        ESP_LOGW(TAG, "stored settings do not parse (%s): using the defaults", bad);
    }
done:
    free(text);
    nvs_close(nvs);
    return ret;
}

esp_err_t routing_settings_store(const routing_settings_t *s)
{
    nvs_handle_t nvs;
    esp_err_t ret = ESP_OK;
    char *text = routing_settings_to_json(s, NULL);

    ESP_RETURN_ON_FALSE(text, ESP_ERR_NO_MEM, TAG, "no memory for the settings text");
    ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "cannot open the %s storage: %s", NVS_NAMESPACE, esp_err_to_name(ret));
        free(text);
        return ret;
    }
    ESP_GOTO_ON_ERROR(nvs_set_str(nvs, NVS_KEY, text), done, TAG, "storing the settings failed");
    ESP_GOTO_ON_ERROR(nvs_commit(nvs), done, TAG, "committing the settings failed");
done:
    nvs_close(nvs);
    free(text);
    return ret;
}
#endif
