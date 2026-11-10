// Decisions of the radio channel mux. Pure: no ESP-IDF calls.
#include "radio_mux_core.h"

#include <stdio.h>
#include <string.h>

#define ID_CHARS_MAX (RADIO_MUX_ID_MAX - 1)
#define BAUD_CLOSE_PERCENT 3

#define CDC_CLASS_COMM 0x02
#define CDC_CLASS_DATA 0x0A

#define VID_SILABS 0x10C4
#define VID_WCH 0x1A86

#define CP210X_FLOW_LIMIT 128
#define CP210X_HANDSHAKE_OFF 0x00000001
#define CP210X_HANDSHAKE_ON 0x00000009
#define CP210X_FLOW_REPLACE_OFF 0x00000040
#define CP210X_FLOW_REPLACE_ON 0x00000080

static const uint32_t accepted_bauds[] = {38400, 57600, 115200, 230400, 460800, 921600, 1000000, 2000000};

static const char *const mode_names[] = {
    [RADIO_MUX_MODE_UNAVAILABLE] = "unavailable",
    [RADIO_MUX_MODE_OFF] = "off",
    [RADIO_MUX_MODE_OTBR] = "otbr",
    [RADIO_MUX_MODE_ZIGGURAT] = "ziggurat",
    [RADIO_MUX_MODE_RCP_PROXY] = "rcp-proxy",
};

static const char *const fw_names[] = {
    [RADIO_MUX_FW_UNKNOWN] = "unknown",
    [RADIO_MUX_FW_SPINEL] = "spinel",
    [RADIO_MUX_FW_ZIGGURAT] = "ziggurat",
    [RADIO_MUX_FW_ZNP] = "znp",
    [RADIO_MUX_FW_EZSP] = "ezsp",
};

// otbr is built in: spinel firmware, one channel, no reboot.
static const radio_mux_rule_t otbr_rule = {
    .mode = RADIO_MUX_MODE_OTBR, .needs = RADIO_MUX_FW_SPINEL, .limit = 1, .reboot_on_change = false,
};

const radio_mux_rule_t *radio_mux_rule_of(const radio_mux_rules_t *rules, radio_mux_mode_t mode)
{
    if (mode == RADIO_MUX_MODE_OTBR) {
        return &otbr_rule;
    }
    if (mode == RADIO_MUX_MODE_OFF || mode == RADIO_MUX_MODE_RCP_PROXY || rules == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < rules->n && i < RADIO_MUX_RULES_MAX; i++) {
        if (rules->rules[i].mode == mode) {
            return &rules->rules[i];
        }
    }
    return NULL;
}

static bool is_alnum(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

void radio_mux_channel_id(uint16_t vid, uint16_t pid, const char *serial, char out[RADIO_MUX_ID_MAX])
{
    size_t pos = (size_t)snprintf(out, RADIO_MUX_ID_MAX, "usb-%04x-%04x", (unsigned)vid, (unsigned)pid);
    bool dashed = false;

    // The vid/pid part is 13 characters, so a dash and a serial character always fit.
    for (size_t i = 0; serial != NULL && serial[i] != '\0' && pos < ID_CHARS_MAX; i++) {
        if (!is_alnum(serial[i])) {
            continue;
        }
        if (!dashed) {
            out[pos++] = '-';
            dashed = true;
        }
        out[pos++] = serial[i];
    }
    out[pos] = '\0';
}

radio_mux_mode_t radio_mux_mode_parse(const char *name)
{
    if (name == NULL) {
        return RADIO_MUX_MODE_INVALID;
    }
    for (int m = RADIO_MUX_MODE_UNAVAILABLE; m < RADIO_MUX_MODE_INVALID; m++) {
        if (strcmp(name, mode_names[m]) == 0) {
            return (radio_mux_mode_t)m;
        }
    }
    return RADIO_MUX_MODE_INVALID;
}

const char *radio_mux_mode_name(radio_mux_mode_t mode)
{
    return (mode >= RADIO_MUX_MODE_UNAVAILABLE && mode < RADIO_MUX_MODE_INVALID) ? mode_names[mode] : NULL;
}

const char *radio_mux_fw_name(radio_mux_fw_t fw)
{
    return (fw >= RADIO_MUX_FW_UNKNOWN && fw <= RADIO_MUX_FW_EZSP) ? fw_names[fw] : fw_names[RADIO_MUX_FW_UNKNOWN];
}

uint32_t radio_mux_available_modes(const radio_mux_rules_t *rules)
{
    uint32_t set = RADIO_MUX_MODE_BIT(RADIO_MUX_MODE_OFF);

    for (size_t i = 0; i < rules->n && i < RADIO_MUX_RULES_MAX; i++) {
        const radio_mux_mode_t m = rules->rules[i].mode;

        if (m == RADIO_MUX_MODE_OTBR || m == RADIO_MUX_MODE_ZIGGURAT || m == RADIO_MUX_MODE_RCP_PROXY) {
            set |= RADIO_MUX_MODE_BIT(m);
        }
    }
    return set;
}

uint32_t radio_mux_reboot_modes(const radio_mux_rules_t *rules)
{
    uint32_t set = 0;

    for (size_t i = 0; i < rules->n && i < RADIO_MUX_RULES_MAX; i++) {
        const radio_mux_rule_t *r = &rules->rules[i];

        if (r->reboot_on_change && r->mode > RADIO_MUX_MODE_UNAVAILABLE && r->mode < RADIO_MUX_MODE_INVALID) {
            set |= RADIO_MUX_MODE_BIT(r->mode);
        }
    }
    return set;
}

bool radio_mux_fits(const radio_mux_rules_t *rules, radio_mux_mode_t mode, radio_mux_fw_t fw)
{
    if (mode == RADIO_MUX_MODE_OFF || mode == RADIO_MUX_MODE_RCP_PROXY) {
        return true;
    }
    const radio_mux_rule_t *rule = radio_mux_rule_of(rules, mode);

    if (rule == NULL) {
        return false;
    }
    return rule->needs == RADIO_MUX_FW_UNKNOWN || rule->needs == fw;
}

radio_mux_mode_t radio_mux_after_detect(const radio_mux_rules_t *rules, radio_mux_mode_t stored, radio_mux_fw_t fw,
                                        char *reason, size_t reason_cap)
{
    if (reason != NULL && reason_cap > 0) {
        reason[0] = '\0';
    }
    if (stored == RADIO_MUX_MODE_UNAVAILABLE || stored == RADIO_MUX_MODE_INVALID) {
        return RADIO_MUX_MODE_OFF;
    }
    if (radio_mux_fits(rules, stored, fw)) {
        return stored;
    }
    if (reason != NULL && reason_cap > 0) {
        const radio_mux_rule_t *rule = radio_mux_rule_of(rules, stored);

        if (rule == NULL) {
            snprintf(reason, reason_cap, "%s is not in this build", radio_mux_mode_name(stored));
        } else {
            snprintf(reason, reason_cap, "stored mode %s needs %s; detected %s", radio_mux_mode_name(stored),
                     radio_mux_fw_name(rule->needs), radio_mux_fw_name(fw));
        }
    }
    return RADIO_MUX_MODE_OFF;
}

void radio_mux_proxy_defaults(radio_mux_proxy_settings_t *out)
{
    memset(out, 0, sizeof(*out));
    out->dir = RADIO_MUX_PROXY_LISTEN;
    out->adapter = RADIO_MUX_PROXY_TCP;
    out->port = RADIO_MUX_PROXY_DEFAULT_PORT;
    out->framing = RADIO_MUX_PROXY_HDLC;
}

radio_mux_driver_t radio_mux_pick_driver(uint16_t vid, uint16_t pid, uint8_t dev_class,
                                         const uint8_t *intf_classes, size_t n_intf)
{
    bool has_data = false;
    bool has_comm = (dev_class == CDC_CLASS_COMM);

    if (vid == VID_SILABS && (pid == 0xEA60 || pid == 0xEA70 || pid == 0xEA71)) {
        return RADIO_MUX_DRV_CP210X;
    }
    if (vid == VID_WCH && (pid == 0x7522 || pid == 0x7523 || pid == 0x5523)) {
        return RADIO_MUX_DRV_CH34X;
    }
    for (size_t i = 0; intf_classes != NULL && i < n_intf; i++) {
        if (intf_classes[i] == CDC_CLASS_DATA) {
            has_data = true;
        } else if (intf_classes[i] == CDC_CLASS_COMM) {
            has_comm = true;
        }
    }
    return (has_data && has_comm) ? RADIO_MUX_DRV_CDC_ACM : RADIO_MUX_DRV_NONE;
}

const char *radio_mux_driver_name(radio_mux_driver_t d)
{
    switch (d) {
    case RADIO_MUX_DRV_CP210X:
        return "cp210x";
    case RADIO_MUX_DRV_CH34X:
        return "ch34x";
    case RADIO_MUX_DRV_CDC_ACM:
        return "cdc_acm";
    case RADIO_MUX_DRV_NONE:
    default:
        return NULL;
    }
}

bool radio_mux_baud_ok(uint32_t baud)
{
    for (size_t i = 0; i < sizeof(accepted_bauds) / sizeof(accepted_bauds[0]); i++) {
        if (baud == accepted_bauds[i]) {
            return true;
        }
    }
    return false;
}

radio_mux_baud_t radio_mux_baud_verdict(uint32_t requested, uint32_t actual)
{
    if (actual == requested) {
        return RADIO_MUX_BAUD_EXACT;
    }
    if (actual == 0) {
        return RADIO_MUX_BAUD_FAR;
    }
    uint64_t diff = actual > requested ? actual - requested : requested - actual;

    return diff * 100 <= (uint64_t)requested * BAUD_CLOSE_PERCENT ? RADIO_MUX_BAUD_CLOSE : RADIO_MUX_BAUD_FAR;
}

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

void radio_mux_cp210x_flow(bool on, uint8_t out[RADIO_MUX_CP210X_FLOW_LEN])
{
    put_le32(&out[0], on ? CP210X_HANDSHAKE_ON : CP210X_HANDSHAKE_OFF);
    put_le32(&out[4], on ? CP210X_FLOW_REPLACE_ON : CP210X_FLOW_REPLACE_OFF);
    put_le32(&out[8], CP210X_FLOW_LIMIT);
    put_le32(&out[12], CP210X_FLOW_LIMIT);
}

/* ---- the attach state machine ---- */

radio_mux_attach_act_t radio_mux_attach_next(radio_mux_attach_t *a, radio_mux_attach_ev_t ev, bool attach_wanted)
{
    switch (a->state) {
    case RADIO_MUX_AS_ABSENT:
        if (ev == RADIO_MUX_AEV_PLUG) {
            a->state = RADIO_MUX_AS_PROBING;
            return RADIO_MUX_ACT_PROBE;
        }
        break;
    case RADIO_MUX_AS_PROBING:
        if (ev == RADIO_MUX_AEV_DETECT_DONE) {
            a->state = RADIO_MUX_AS_REOPENING;
            return RADIO_MUX_ACT_REOPEN;
        }
        if (ev == RADIO_MUX_AEV_UNPLUG) {
            a->state = RADIO_MUX_AS_CLOSING;
            return RADIO_MUX_ACT_CLOSE;
        }
        break;
    case RADIO_MUX_AS_REOPENING:
        if (ev == RADIO_MUX_AEV_REOPENED) {
            a->state = attach_wanted ? RADIO_MUX_AS_ATTACHED : RADIO_MUX_AS_IDLE;
            return attach_wanted ? RADIO_MUX_ACT_ATTACH : RADIO_MUX_ACT_NONE;
        }
        if (ev == RADIO_MUX_AEV_UNPLUG) {
            a->state = RADIO_MUX_AS_CLOSING;
            return RADIO_MUX_ACT_CLOSE;
        }
        break;
    case RADIO_MUX_AS_IDLE:
        if (ev == RADIO_MUX_AEV_MODE_SET && attach_wanted) {
            a->state = RADIO_MUX_AS_ATTACHED;
            return RADIO_MUX_ACT_ATTACH;
        }
        if (ev == RADIO_MUX_AEV_UNPLUG) {
            a->state = RADIO_MUX_AS_CLOSING;
            return RADIO_MUX_ACT_CLOSE;
        }
        break;
    case RADIO_MUX_AS_ATTACHED:
        if (ev == RADIO_MUX_AEV_MODE_SET) {
            a->state = attach_wanted ? RADIO_MUX_AS_ATTACHED : RADIO_MUX_AS_IDLE;
            return attach_wanted ? RADIO_MUX_ACT_REATTACH : RADIO_MUX_ACT_DETACH;
        }
        if (ev == RADIO_MUX_AEV_ATTACH_FAILED) {
            a->state = RADIO_MUX_AS_IDLE;
            return RADIO_MUX_ACT_NONE;
        }
        if (ev == RADIO_MUX_AEV_UNPLUG) {
            a->state = RADIO_MUX_AS_CLOSING;
            return RADIO_MUX_ACT_GONE_CLOSE;
        }
        break;
    case RADIO_MUX_AS_CLOSING:
        if (ev == RADIO_MUX_AEV_PLUG) {
            a->plug_pending = true;
            return RADIO_MUX_ACT_DEFER;
        }
        if (ev == RADIO_MUX_AEV_UNPLUG) { // the deferred device left again
            a->plug_pending = false;
            return RADIO_MUX_ACT_NONE;
        }
        if (ev == RADIO_MUX_AEV_CLOSED) {
            const bool pending = a->plug_pending;

            a->plug_pending = false;
            a->state = pending ? RADIO_MUX_AS_PROBING : RADIO_MUX_AS_ABSENT;
            return pending ? RADIO_MUX_ACT_PROBE : RADIO_MUX_ACT_NONE;
        }
        break;
    }
    return RADIO_MUX_ACT_NONE;
}

bool radio_mux_attach_wanted(const radio_mux_rules_t *rules, const radio_mux_chan_view_t *ch,
                             bool consumer_registered)
{
    if (!ch->present || !ch->supported || !consumer_registered) {
        return false;
    }
    if (ch->mode == RADIO_MUX_MODE_OFF || ch->mode == RADIO_MUX_MODE_UNAVAILABLE ||
            ch->mode == RADIO_MUX_MODE_INVALID) {
        return false;
    }
    return radio_mux_fits(rules, ch->mode, ch->fw);
}

radio_mux_post_t radio_mux_post_rx(bool queue_full, size_t len, radio_mux_counters_t *c)
{
    if (queue_full) {
        c->drops++;
        return RADIO_MUX_DROPPED;
    }
    c->frames_in++;
    c->bytes_in += len;
    return RADIO_MUX_POSTED;
}

#define FNV_OFFSET 0x811C9DC5u
#define FNV_PRIME 0x01000193u

bool radio_mux_usb_external_refuses(bool external, radio_mux_kind_t kind, bool mode_change)
{
    return external && kind == RADIO_MUX_KIND_USB && mode_change;
}

bool radio_mux_is_builtin_id(const char *id)
{
    return strcmp(id, RADIO_MUX_BUILTIN_ID) == 0 || strcmp(id, RADIO_MUX_BUILTIN_NVS_ID) == 0;
}

const char *radio_mux_nvs_id(const char *id)
{
    return radio_mux_is_builtin_id(id) ? RADIO_MUX_BUILTIN_NVS_ID : id;
}

void radio_mux_nvs_key(const char *id, char out[16])
{
    uint32_t h = FNV_OFFSET;

    if (radio_mux_is_builtin_id(id)) {
        snprintf(out, 16, "%s", RADIO_MUX_BUILTIN_NVS_ID);
        return;
    }
    for (size_t i = 0; id[i] != '\0'; i++) {
        h ^= (uint8_t)id[i];
        h *= FNV_PRIME;
    }
    snprintf(out, 16, "u%08lx", (unsigned long)h);
}

const char *radio_mux_state_name(radio_mux_state_t state)
{
    switch (state) {
    case RADIO_MUX_STATE_IDLE:
        return "idle";
    case RADIO_MUX_STATE_STARTING:
        return "starting";
    case RADIO_MUX_STATE_RUNNING:
        return "running";
    case RADIO_MUX_STATE_FAULT:
        return "fault";
    case RADIO_MUX_STATE_BUSY:
        return "busy";
    default:
        return NULL;
    }
}

const char *radio_mux_fw_source_name(radio_mux_fw_source_t src)
{
    switch (src) {
    case RADIO_MUX_FW_SRC_PROBE:
        return "probe";
    case RADIO_MUX_FW_SRC_PASSIVE:
        return "passive";
    default:
        return NULL;
    }
}

#define PORT_HTTP 80
#define PORT_BGP 179
#define PROXY_HOST_NAME_MAX_LEN 63
#define LABEL_MAX_LEN 63
#define IP4_PARTS 4
#define IP4_PART_MAX 255
#define IP6_GROUPS 8
#define IP6_GROUP_DIGITS 4

static bool is_digit(char c)
{
    return c >= '0' && c <= '9';
}

static bool is_hex(char c)
{
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
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
    if (n == 0 || n > PROXY_HOST_NAME_MAX_LEN) {
        return false;
    }
    if (strchr(host, ':')) {
        return ip6_ok(host);
    }
    return ip4_ok(host, n) || name_ok(host, n);
}

radio_mux_proxy_check_t radio_mux_proxy_check(const radio_mux_proxy_settings_t *s)
{
    if ((s->dir != RADIO_MUX_PROXY_LISTEN && s->dir != RADIO_MUX_PROXY_CONNECT) ||
            (s->adapter != RADIO_MUX_PROXY_TCP && s->adapter != RADIO_MUX_PROXY_UDP) ||
            (s->framing != RADIO_MUX_PROXY_HDLC && s->framing != RADIO_MUX_PROXY_RAW)) {
        return RADIO_MUX_PROXY_CHECK_BAD_KIND;
    }
    if (s->port == 0 || s->port == PORT_HTTP || s->port == PORT_BGP) {
        return RADIO_MUX_PROXY_CHECK_BAD_PORT;
    }
    const size_t n = strnlen(s->host, sizeof(s->host));

    if (n == sizeof(s->host)) {
        return RADIO_MUX_PROXY_CHECK_BAD_HOST; // no terminator
    }
    if (s->dir == RADIO_MUX_PROXY_CONNECT && n == 0) {
        return RADIO_MUX_PROXY_CHECK_NEED_HOST;
    }
    if (n > 0 && !host_ok(s->host, n)) {
        return RADIO_MUX_PROXY_CHECK_BAD_HOST;
    }
    return RADIO_MUX_PROXY_CHECK_OK;
}

radio_mux_proxy_check_t radio_mux_proxy_merge(const radio_mux_proxy_settings_t *stored, uint8_t fields,
                                              const radio_mux_proxy_settings_t *given, radio_mux_proxy_settings_t *out)
{
    *out = *stored;
    if (fields == 0) {
        return RADIO_MUX_PROXY_CHECK_OK;
    }
    if (fields & RADIO_MUX_PROXY_F_DIR) {
        out->dir = given->dir;
    }
    if (fields & RADIO_MUX_PROXY_F_ADAPTER) {
        out->adapter = given->adapter;
    }
    if (fields & RADIO_MUX_PROXY_F_PORT) {
        out->port = given->port;
    }
    if (fields & RADIO_MUX_PROXY_F_HOST) {
        memcpy(out->host, given->host, sizeof(out->host));
    }
    if (fields & RADIO_MUX_PROXY_F_FRAMING) {
        out->framing = given->framing;
    }
    return radio_mux_proxy_check(out);
}

#define TX_BITS_PER_BYTE 10 // 8N1: start, 8 data, stop
#define TX_MARGIN 2
#define TX_SLOWEST_BAUD 38400
#define MS_PER_S 1000

uint32_t radio_mux_tx_timeout_ms(size_t n, uint32_t baud)
{
    const uint64_t b = baud ? baud : TX_SLOWEST_BAUD;
    const uint64_t bits_ms = (uint64_t)n * TX_BITS_PER_BYTE * TX_MARGIN * MS_PER_S;

    return RADIO_MUX_TX_BASE_MS + (uint32_t)((bits_ms + b - 1) / b);
}

esp_err_t radio_mux_tx_transport_err(esp_err_t err)
{
    return err == ESP_ERR_INVALID_STATE ? ESP_FAIL : err;
}

bool radio_mux_reset_count_request(radio_mux_reset_count_t *c)
{
    if (++c->unanswered < RADIO_MUX_USB_DEAD_RESETS) {
        return false;
    }
    c->unanswered = 0;
    return true;
}

void radio_mux_reset_count_clear(radio_mux_reset_count_t *c)
{
    c->unanswered = 0;
}

bool radio_mux_ch34x_uart_error(const uint8_t *pkt, size_t len)
{
    return pkt != NULL && len >= RADIO_MUX_CH34X_STATUS_LEN && (pkt[0] & RADIO_MUX_CH34X_UART_ERR_MASK) != 0;
}

bool radio_mux_port_frames(bool c6_port, bool consumer_frames)
{
    return c6_port || consumer_frames;
}

/* ---- the C6 over SPI ---- */

static void put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}

static uint16_t get_le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

#define SPI_IDX_FLAG 0
#define SPI_IDX_ACCEPT 1
#define SPI_IDX_DATA 3

void radio_mux_spi_hdr_encode(const radio_mux_spi_hdr_t *h, uint8_t out[RADIO_MUX_SPI_HDR_LEN])
{
    out[SPI_IDX_FLAG] = RADIO_MUX_SPI_FLAG_PATTERN | (h->reset ? RADIO_MUX_SPI_FLAG_RESET : 0);
    put_le16(&out[SPI_IDX_ACCEPT], h->accept_len);
    put_le16(&out[SPI_IDX_DATA], h->data_len);
}

bool radio_mux_spi_hdr_decode(const uint8_t in[RADIO_MUX_SPI_HDR_LEN], radio_mux_spi_hdr_t *out)
{
    if ((in[SPI_IDX_FLAG] & RADIO_MUX_SPI_PATTERN_MASK) != RADIO_MUX_SPI_FLAG_PATTERN) {
        return false;
    }
    out->reset = (in[SPI_IDX_FLAG] & RADIO_MUX_SPI_FLAG_RESET) != 0;
    out->accept_len = get_le16(&in[SPI_IDX_ACCEPT]);
    out->data_len = get_le16(&in[SPI_IDX_DATA]);
    return true;
}

void radio_mux_spi_check(const uint8_t rx_hdr[RADIO_MUX_SPI_HDR_LEN], uint16_t tx_len, uint16_t accept,
                         uint16_t frame_max, radio_mux_spi_result_t *out)
{
    radio_mux_spi_hdr_t h;

    memset(out, 0, sizeof(*out));
    if (!radio_mux_spi_hdr_decode(rx_hdr, &h) || h.accept_len > frame_max || h.data_len > frame_max) {
        return;
    }
    out->valid = true;
    out->rcp_reset = h.reset;
    // As the RCP's own rule (ncp_spi.cpp): a frame counts as sent when its length fits the other side's accept.
    out->rx_len = h.data_len <= accept ? h.data_len : 0;
    out->tx_taken = tx_len == 0 || tx_len <= h.accept_len;
}

size_t radio_mux_spi_xfer_len(uint16_t tx_len, uint16_t accept)
{
    const size_t n = RADIO_MUX_SPI_HDR_LEN + (size_t)(tx_len > accept ? tx_len : accept);

    return (n + RADIO_MUX_SPI_XFER_ALIGN - 1) / RADIO_MUX_SPI_XFER_ALIGN * RADIO_MUX_SPI_XFER_ALIGN;
}

uint32_t radio_mux_spi_gap_wait_us(int64_t now_us, int64_t last_end_us, uint32_t gap_us)
{
    if (last_end_us < 0) {
        return 0;
    }
    if (now_us < last_end_us) {
        return gap_us;
    }
    const int64_t since = now_us - last_end_us;

    return since >= (int64_t)gap_us ? 0 : (uint32_t)((int64_t)gap_us - since);
}

uint32_t radio_mux_gen_next(uint32_t gen)
{
    return gen >= RADIO_MUX_GEN_MAX ? 1 : gen + 1;
}

uintptr_t radio_mux_handle_pack(size_t slot, uint32_t gen)
{
    if (slot >= RADIO_MUX_HANDLE_SLOTS || gen == 0 || gen > RADIO_MUX_GEN_MAX) {
        return 0;
    }
    return ((uintptr_t)gen << RADIO_MUX_HANDLE_SLOT_BITS) | (uintptr_t)slot;
}

bool radio_mux_handle_unpack(uintptr_t h, size_t *slot, uint32_t *gen)
{
    const uintptr_t g = h >> RADIO_MUX_HANDLE_SLOT_BITS;

    if (g == 0 || g > RADIO_MUX_GEN_MAX) {
        return false;
    }
    *slot = (size_t)(h & (RADIO_MUX_HANDLE_SLOTS - 1));
    *gen = (uint32_t)g;
    return true;
}

bool radio_mux_handle_current(uint32_t gen, uint32_t current)
{
    return current != 0 && gen == current;
}

void radio_mux_close_retry_start(radio_mux_close_retry_t *r, int64_t now_ms)
{
    r->pending = true;
    r->gave_up = false;
    r->failures = 0;
    r->next_ms = now_ms + RADIO_MUX_CLOSE_RETRY_MS;
}

radio_mux_close_act_t radio_mux_close_retry_due(const radio_mux_close_retry_t *r, int64_t now_ms)
{
    if (!r->pending) {
        return RADIO_MUX_CLOSE_IDLE;
    }
    return now_ms >= r->next_ms ? RADIO_MUX_CLOSE_TRY : RADIO_MUX_CLOSE_WAIT;
}

radio_mux_close_act_t radio_mux_close_retry_result(radio_mux_close_retry_t *r, int64_t now_ms, bool closed)
{
    if (closed) {
        r->pending = false;
        r->gave_up = false;
        r->failures = 0;
        return RADIO_MUX_CLOSE_DONE;
    }
    if (++r->failures >= RADIO_MUX_CLOSE_RETRIES) {
        r->pending = false;
        r->gave_up = true;
        return RADIO_MUX_CLOSE_GAVE_UP;
    }
    r->next_ms = now_ms + RADIO_MUX_CLOSE_RETRY_MS;
    return RADIO_MUX_CLOSE_WAIT;
}

bool radio_mux_usb_error_due(bool flagged, int64_t last_us, int64_t now_us)
{
    if (!flagged) {
        return false;
    }
    return last_us == 0 || now_us < last_us || now_us - last_us >= RADIO_MUX_USB_ERROR_REOPEN_US;
}

bool radio_mux_reset_applies(radio_mux_attach_state_t state, bool leased, bool on_port, bool current)
{
    return on_port && current && (state == RADIO_MUX_AS_ATTACHED || leased);
}

size_t radio_mux_evict_pick(const radio_mux_slot_view_t *slots, size_t n, bool for_unsupported)
{
    size_t pick = n;

    for (size_t i = 0; i < n; i++) {
        const radio_mux_slot_view_t *s = &slots[i];

        if (!s->used) {
            return i;
        }
        if (s->present || s->on_port || s->mode != RADIO_MUX_MODE_OFF || (for_unsupported && s->remembered)) {
            continue;
        }
        if (pick == n || s->last_seen < slots[pick].last_seen) {
            pick = i;
        }
    }
    return pick;
}

bool radio_mux_detect_keeps_record(radio_mux_fw_t stored, radio_mux_fw_t detected)
{
    return detected == RADIO_MUX_FW_UNKNOWN && stored != RADIO_MUX_FW_UNKNOWN;
}
