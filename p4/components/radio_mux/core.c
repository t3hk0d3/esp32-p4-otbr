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

// The rule of a mode that has one: otbr, or a registered consumer. NULL for off, rcp-proxy and unregistered modes.
static const radio_mux_rule_t *find_rule(const radio_mux_rules_t *rules, radio_mux_mode_t mode)
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

bool radio_mux_fits(const radio_mux_rules_t *rules, radio_mux_mode_t mode, radio_mux_fw_t fw)
{
    if (mode == RADIO_MUX_MODE_OFF || mode == RADIO_MUX_MODE_RCP_PROXY) {
        return true;
    }
    const radio_mux_rule_t *rule = find_rule(rules, mode);

    if (rule == NULL) {
        return false;
    }
    return rule->needs == RADIO_MUX_FW_UNKNOWN || rule->needs == fw;
}

static bool mode_used_by_other(const radio_mux_chan_view_t *chans, size_t n, size_t target, radio_mux_mode_t mode,
                               size_t *first_index, size_t *count)
{
    *count = 0;
    for (size_t i = 0; i < n; i++) {
        if (i != target && chans[i].mode == mode) {
            if (*count == 0 && first_index != NULL) {
                *first_index = i;
            }
            (*count)++;
        }
    }
    return *count > 0;
}

radio_mux_verdict_t radio_mux_check_set_mode(const radio_mux_rules_t *rules, const radio_mux_chan_view_t *chans,
                                             size_t n, size_t target, radio_mux_mode_t want, size_t *move_from)
{
    size_t first = 0, count = 0;

    if (want == RADIO_MUX_MODE_UNAVAILABLE || want == RADIO_MUX_MODE_INVALID) {
        return RADIO_MUX_REFUSE_BAD_VALUE;
    }
    if (target >= n || !chans[target].present || !chans[target].supported) {
        return RADIO_MUX_REFUSE_UNAVAILABLE;
    }
    const radio_mux_rule_t *rule = find_rule(rules, want);

    if (want != RADIO_MUX_MODE_OFF && want != RADIO_MUX_MODE_RCP_PROXY && rule == NULL) {
        return RADIO_MUX_REFUSE_NOT_AVAILABLE;
    }
    if (!radio_mux_fits(rules, want, chans[target].fw)) {
        return RADIO_MUX_REFUSE_INCOMPATIBLE;
    }
    // otbr and a registered ziggurat consumer never run together.
    if ((want == RADIO_MUX_MODE_OTBR &&
            mode_used_by_other(chans, n, target, RADIO_MUX_MODE_ZIGGURAT, NULL, &count)) ||
            (want == RADIO_MUX_MODE_ZIGGURAT &&
             mode_used_by_other(chans, n, target, RADIO_MUX_MODE_OTBR, NULL, &count))) {
        return RADIO_MUX_REFUSE_HYBRID;
    }
    if (rule != NULL && rule->limit > 0 && mode_used_by_other(chans, n, target, want, &first, &count) &&
            count >= rule->limit) {
        if (want == RADIO_MUX_MODE_OTBR) {
            if (move_from != NULL) {
                *move_from = first;
            }
            return RADIO_MUX_MOVE;
        }
        return RADIO_MUX_REFUSE_MODE_TAKEN;
    }
    const radio_mux_rule_t *leaving = find_rule(rules, chans[target].mode);

    if (chans[target].mode != want && ((rule != NULL && rule->reboot_on_change) ||
                                       (leaving != NULL && leaving->reboot_on_change))) {
        return RADIO_MUX_APPLY_REBOOT;
    }
    return RADIO_MUX_APPLY;
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
        const radio_mux_rule_t *rule = find_rule(rules, stored);

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

void radio_mux_nvs_key(const char *id, char out[16])
{
    uint32_t h = FNV_OFFSET;

    if (strcmp(id, "c6") == 0) {
        snprintf(out, 16, "c6");
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

bool radio_mux_proxy_ok(const radio_mux_proxy_settings_t *s)
{
    const size_t host_len = strnlen(s->host, sizeof(s->host));

    if ((s->dir != RADIO_MUX_PROXY_LISTEN && s->dir != RADIO_MUX_PROXY_CONNECT) ||
            (s->adapter != RADIO_MUX_PROXY_TCP && s->adapter != RADIO_MUX_PROXY_UDP) ||
            (s->framing != RADIO_MUX_PROXY_HDLC && s->framing != RADIO_MUX_PROXY_RAW)) {
        return false;
    }
    if (s->port == 0 || s->port == PORT_HTTP || s->port == PORT_BGP || host_len == sizeof(s->host)) {
        return false;
    }
    return s->dir != RADIO_MUX_PROXY_CONNECT || host_len > 0;
}
