// /device/channels bodies and decisions (spec docs/superpowers/specs/2026-11-06-radio-channel-mux-design.md, section
// API). Pure.
#include "channels_json.h"

#include <stdio.h>
#include <string.h>

#include "br_core_mode.h"
#include "cJSON.h"

// The mux took the change but did not answer in time (radio_mux_set's REFUSE_PENDING, radio_mux_forget's
// ESP_ERR_NOT_FINISHED): it is not refused. A mode, speed or proxy change may still be applied; a start, move or stop
// of Thread never is (the mover is not called once the caller has been answered).
#define MUX_BUSY_DETAIL "the radio mux is busy; a settings change may still apply, a start, move or stop will not"

/* ---- GET ---- */

// "" (or NULL) is written as null: no value.
static void str_or_null(json_out_t *w, const char *text)
{
    json_out_str(w, text && text[0] ? text : NULL);
}

static void hex4(json_out_t *w, uint16_t v)
{
    char text[8];

    snprintf(text, sizeof(text), "%04x", v);
    json_out_str(w, text);
}

static void usb_json(json_out_t *w, const radio_mux_chan_info_t *c)
{
    if (c->kind != RADIO_MUX_KIND_USB) {
        json_out_raw(w, "null");
        return;
    }
    json_out_raw(w, "{\"vid\":");
    hex4(w, c->usb.vid);
    json_out_raw(w, ",\"pid\":");
    hex4(w, c->usb.pid);
    json_out_raw(w, ",\"serial\":");
    str_or_null(w, c->usb.serial);
    json_out_raw(w, ",\"driver\":");
    json_out_str(w, radio_mux_driver_name(c->usb.driver)); // NULL for NONE
    json_out_raw(w, "}");
}

static void proxy_json(json_out_t *w, const radio_mux_chan_info_t *c)
{
    const radio_mux_proxy_settings_t *p = &c->proxy;

    if (c->kind == RADIO_MUX_KIND_USB && !c->supported) {
        json_out_raw(w, "null"); // no driver: no mode runs on it
        return;
    }
    json_out_raw(w, "{\"direction\":");
    json_out_str(w, p->dir == RADIO_MUX_PROXY_CONNECT ? "connect" : "listen");
    json_out_raw(w, ",\"adapter\":");
    json_out_str(w, p->adapter == RADIO_MUX_PROXY_UDP ? "udp" : "tcp");
    json_out_raw(w, ",\"port\":");
    json_out_uint(w, p->port);
    json_out_raw(w, ",\"host\":");
    json_out_str(w, p->host);
    json_out_raw(w, ",\"framing\":");
    json_out_str(w, p->framing == RADIO_MUX_PROXY_RAW ? "raw" : "hdlc");
    json_out_raw(w, ",\"state\":");
    str_or_null(w, c->proxy_state);
    json_out_raw(w, ",\"peer\":");
    str_or_null(w, c->proxy_peer);
    json_out_raw(w, ",\"last_error\":");
    str_or_null(w, c->proxy_last_error);
    json_out_raw(w, "}");
}

// The channel's error: its own reason, else, while its proxy backs off after a failed open or connect, why it failed.
static const char *channel_error(const radio_mux_chan_info_t *c)
{
    if (c->reason[0] == '\0' && c->proxy_state && strcmp(c->proxy_state, "backoff") == 0) {
        return c->proxy_last_error;
    }
    return c->reason;
}

static void firmware_json(json_out_t *w, const radio_mux_firmware_t *f)
{
    if (f->source == RADIO_MUX_FW_SRC_NONE) {
        json_out_raw(w, "null");
        return;
    }
    json_out_raw(w, "{\"type\":");
    json_out_str(w, radio_mux_fw_name(f->rec.type));
    json_out_raw(w, ",\"version\":");
    str_or_null(w, f->rec.version);
    json_out_raw(w, ",\"chip\":");
    str_or_null(w, f->rec.chip);
    json_out_raw(w, ",\"baud\":");
    if (f->rec.baud) {
        json_out_uint(w, f->rec.baud);
    } else {
        json_out_raw(w, "null");
    }
    json_out_raw(w, ",\"source\":");
    json_out_str(w, radio_mux_fw_source_name(f->source));
    json_out_raw(w, ",\"at\":");
    if (f->at > 0) {
        json_out_u64(w, (uint64_t)f->at);
    } else {
        json_out_raw(w, "null"); // the board clock was not set
    }
    json_out_raw(w, "}");
}

static void counters_json(json_out_t *w, const radio_mux_counters_t *k)
{
    json_out_raw(w, "{\"frames_in\":");
    json_out_uint(w, k->frames_in);
    json_out_raw(w, ",\"frames_out\":");
    json_out_uint(w, k->frames_out);
    json_out_raw(w, ",\"bytes_in\":");
    json_out_u64(w, k->bytes_in);
    json_out_raw(w, ",\"bytes_out\":");
    json_out_u64(w, k->bytes_out);
    json_out_raw(w, ",\"drops\":");
    json_out_uint(w, k->drops);
    json_out_raw(w, ",\"resets\":");
    json_out_uint(w, k->resets);
    json_out_raw(w, ",\"rx_errors\":");
    json_out_uint(w, k->rx_errors);
    json_out_raw(w, ",\"uart_errors\":");
    json_out_uint(w, k->uart_errors);
    json_out_raw(w, "}");
}

// The channel's fields, without the closing brace.
static void channel_fields(json_out_t *w, const radio_mux_chan_info_t *c)
{
    json_out_raw(w, "{\"id\":");
    json_out_str(w, c->id);
    json_out_raw(w, ",\"kind\":");
    json_out_str(w, c->kind == RADIO_MUX_KIND_C6 ? "c6" : "usb");
    json_out_raw(w, ",\"label\":");
    json_out_str(w, c->label);
    json_out_raw(w, ",\"mode\":");
    json_out_str(w, radio_mux_mode_name(c->mode));
    json_out_raw(w, ",\"stored_mode\":");
    json_out_str(w, radio_mux_mode_name(c->stored_mode));
    json_out_raw(w, ",\"state\":");
    json_out_str(w, radio_mux_state_name(c->state));
    json_out_raw(w, ",\"error\":");
    str_or_null(w, channel_error(c));
    json_out_raw(w, ",\"line\":{\"baud\":");
    json_out_uint(w, c->line.baud);
    json_out_raw(w, ",\"flow_control\":");
    json_out_bool(w, c->line.flow_control);
    json_out_raw(w, "},\"usb\":");
    usb_json(w, c);
    json_out_raw(w, ",\"caps\":{\"firmware_update\":");
    json_out_bool(w, c->caps.firmware_update);
    json_out_raw(w, ",\"hw_reset\":");
    json_out_bool(w, c->caps.hw_reset);
    json_out_raw(w, ",\"flow_control\":");
    json_out_bool(w, c->caps.flow_control);
    json_out_raw(w, "},\"rcp_proxy\":");
    proxy_json(w, c);
    json_out_raw(w, ",\"firmware\":");
    firmware_json(w, &c->firmware);
    json_out_raw(w, ",\"counters\":");
    counters_json(w, &c->counters);
}

int device_api_channels_json(char *buf, size_t cap, const radio_mux_snapshot_t *s)
{
    json_out_t w;

    json_out_init(&w, buf, cap);
    json_out_raw(&w, "{\"available_modes\":[");
    bool first = true;

    for (radio_mux_mode_t m = RADIO_MUX_MODE_OFF; m < RADIO_MUX_MODE_INVALID; m++) {
        if (s->available_modes & RADIO_MUX_MODE_BIT(m)) {
            json_out_raw(&w, first ? "" : ",");
            json_out_str(&w, radio_mux_mode_name(m));
            first = false;
        }
    }
    json_out_raw(&w, "],\"channels\":[");
    for (size_t i = 0; i < s->n && i < RADIO_MUX_CHANNELS_MAX; i++) {
        if (i > 0) {
            json_out_raw(&w, ",");
        }
        channel_fields(&w, &s->chans[i]);
        json_out_raw(&w, "}");
    }
    json_out_raw(&w, "]}");
    return w.overflow ? -1 : (int)w.len;
}

int device_api_channel_json(char *buf, size_t cap, const radio_mux_chan_info_t *c, bool reboot)
{
    json_out_t w;

    json_out_init(&w, buf, cap);
    channel_fields(&w, c);
    json_out_raw(&w, reboot ? ",\"reboot\":true}" : "}");
    return w.overflow ? -1 : (int)w.len;
}

int device_api_channel_moving_json(char *buf, size_t cap, const char *moved_from)
{
    json_out_t w;

    json_out_init(&w, buf, cap);
    json_out_raw(&w, "{\"moving\":true,\"moved_from\":");
    str_or_null(&w, moved_from);
    json_out_raw(&w, "}");
    return w.overflow ? -1 : (int)w.len;
}

/* ---- PUT ---- */

// A whole number in [min, max] (cJSON keeps numbers as doubles).
static bool whole_in(const cJSON *item, double min, double max, uint32_t *out)
{
    if (!cJSON_IsNumber(item)) {
        return false;
    }
    const double v = item->valuedouble;

    if (v < min || v > max || v != (double)(uint32_t)v) {
        return false;
    }
    *out = (uint32_t)v;
    return true;
}

// The rcp_proxy object; NULL on success, else the field.
static const char *parse_proxy(const cJSON *obj, radio_mux_set_req_t *r)
{
    const cJSON *item;

    if (!cJSON_IsObject(obj)) {
        return "rcp_proxy";
    }
    cJSON_ArrayForEach(item, obj) {
        const char *key = item->string;

        if (strcmp(key, "direction") == 0) {
            const char *v = cJSON_GetStringValue(item);

            if (v && strcmp(v, "listen") == 0) {
                r->proxy.dir = RADIO_MUX_PROXY_LISTEN;
            } else if (v && strcmp(v, "connect") == 0) {
                r->proxy.dir = RADIO_MUX_PROXY_CONNECT;
            } else {
                return "rcp_proxy.direction";
            }
            r->proxy_fields |= RADIO_MUX_PROXY_F_DIR;
        } else if (strcmp(key, "adapter") == 0) {
            const char *v = cJSON_GetStringValue(item);

            if (v && strcmp(v, "tcp") == 0) {
                r->proxy.adapter = RADIO_MUX_PROXY_TCP;
            } else if (v && strcmp(v, "udp") == 0) {
                r->proxy.adapter = RADIO_MUX_PROXY_UDP;
            } else {
                return "rcp_proxy.adapter";
            }
            r->proxy_fields |= RADIO_MUX_PROXY_F_ADAPTER;
        } else if (strcmp(key, "port") == 0) {
            uint32_t port;

            if (!whole_in(item, 1, 65535, &port)) {
                return "rcp_proxy.port";
            }
            r->proxy.port = (uint16_t)port;
            r->proxy_fields |= RADIO_MUX_PROXY_F_PORT;
        } else if (strcmp(key, "host") == 0) {
            const char *v = cJSON_GetStringValue(item);

            if (!v || strlen(v) >= sizeof(r->proxy.host)) {
                return "rcp_proxy.host";
            }
            strlcpy(r->proxy.host, v, sizeof(r->proxy.host));
            r->proxy_fields |= RADIO_MUX_PROXY_F_HOST;
        } else if (strcmp(key, "framing") == 0) {
            const char *v = cJSON_GetStringValue(item);

            if (v && strcmp(v, "hdlc") == 0) {
                r->proxy.framing = RADIO_MUX_PROXY_HDLC;
            } else if (v && strcmp(v, "raw") == 0) {
                r->proxy.framing = RADIO_MUX_PROXY_RAW;
            } else {
                return "rcp_proxy.framing";
            }
            r->proxy_fields |= RADIO_MUX_PROXY_F_FRAMING;
        } else if (strcmp(key, "kick") == 0) {
            if (!cJSON_IsBool(item)) {
                return "rcp_proxy.kick";
            }
            r->kick = cJSON_IsTrue(item);
        } else {
            static char unknown[48]; // valid until the next call (the HTTP server task only)

            snprintf(unknown, sizeof(unknown), "rcp_proxy.%s", key);
            return unknown;
        }
    }
    r->has_proxy = r->proxy_fields != 0;
    return NULL;
}

// The top level; NULL on success, else the field.
static const char *parse_fields(const cJSON *json, radio_mux_set_req_t *r)
{
    const cJSON *item;

    cJSON_ArrayForEach(item, json) {
        const char *key = item->string;

        if (strcmp(key, "mode") == 0) {
            const radio_mux_mode_t m = radio_mux_mode_parse(cJSON_GetStringValue(item));

            if (m == RADIO_MUX_MODE_INVALID || m == RADIO_MUX_MODE_UNAVAILABLE) {
                return "mode"; // unavailable is reported, never set
            }
            r->has_mode = true;
            r->mode = m;
        } else if (strcmp(key, "baud") == 0) {
            if (!whole_in(item, 1, UINT32_MAX, &r->baud) || !radio_mux_baud_ok(r->baud)) {
                return "baud";
            }
            r->has_baud = true;
        } else if (strcmp(key, "flow_control") == 0) {
            if (!cJSON_IsBool(item)) {
                return "flow_control";
            }
            r->has_flow = true;
            r->flow = cJSON_IsTrue(item);
        } else if (strcmp(key, "rcp_proxy") == 0) {
            const char *bad = parse_proxy(item, r);

            if (bad) {
                return bad;
            }
        } else {
            return key;
        }
    }
    return NULL;
}

esp_err_t device_api_channel_put_parse(const char *body, radio_mux_set_req_t *out, const char **bad_field)
{
    static char bad_copy[48]; // the field outlives the parsed body
    cJSON *json = cJSON_Parse(body);
    radio_mux_set_req_t r = {0};
    const char *bad = NULL;

    radio_mux_proxy_defaults(&r.proxy);
    if (!cJSON_IsObject(json)) {
        bad = "body";
    } else {
        bad = parse_fields(json, &r);
        if (bad) {
            strlcpy(bad_copy, bad, sizeof(bad_copy));
            bad = bad_copy;
        }
    }
    cJSON_Delete(json);
    *bad_field = bad;
    if (bad) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = r;
    return ESP_OK;
}

// radio_mux_proxy_merge of the request's rcp_proxy settings over `stored` (the one merge and check, also the mux's).
static radio_mux_proxy_check_t merged_proxy(const radio_mux_proxy_settings_t *stored, const radio_mux_set_req_t *req,
                                            radio_mux_proxy_settings_t *out)
{
    return radio_mux_proxy_merge(stored, req->has_proxy ? req->proxy_fields : 0, &req->proxy, out);
}

const char *device_api_channel_proxy_refusal(const radio_mux_proxy_settings_t *stored, const radio_mux_set_req_t *req)
{
    radio_mux_proxy_settings_t m;

    switch (merged_proxy(stored, req, &m)) {
    case RADIO_MUX_PROXY_CHECK_OK:
        return NULL;
    case RADIO_MUX_PROXY_CHECK_BAD_KIND:
        return "rcp_proxy"; // the parser only gives known names: not reached from a PUT
    case RADIO_MUX_PROXY_CHECK_BAD_PORT:
        return "rcp_proxy.port";
    case RADIO_MUX_PROXY_CHECK_NEED_HOST:
    case RADIO_MUX_PROXY_CHECK_BAD_HOST:
    default:
        return "rcp_proxy.host";
    }
}

static bool listens_on(radio_mux_mode_t mode, const radio_mux_proxy_settings_t *p, radio_mux_proxy_adapter_t adapter,
                       uint16_t port)
{
    return mode == RADIO_MUX_MODE_RCP_PROXY && p->dir == RADIO_MUX_PROXY_LISTEN && p->adapter == adapter &&
           p->port == port;
}

const char *device_api_channel_port_conflict(const radio_mux_snapshot_t *s, const char *id, const radio_mux_set_req_t *req)
{
    const radio_mux_chan_info_t *self = NULL;

    for (size_t i = 0; i < s->n && i < RADIO_MUX_CHANNELS_MAX; i++) {
        if (strcmp(s->chans[i].id, id) == 0) {
            self = &s->chans[i];
        }
    }
    if (!self) {
        return NULL;
    }
    const radio_mux_mode_t mode = req->has_mode ? req->mode : self->stored_mode;
    radio_mux_proxy_settings_t want;

    merged_proxy(&self->proxy, req, &want); // settings it refuses were answered 400 before (proxy_refusal)

    if (!listens_on(mode, &want, want.adapter, want.port)) {
        return NULL;
    }
    for (size_t i = 0; i < s->n && i < RADIO_MUX_CHANNELS_MAX; i++) {
        const radio_mux_chan_info_t *c = &s->chans[i];

        if (c != self && listens_on(c->stored_mode, &c->proxy, want.adapter, want.port)) {
            return c->id;
        }
    }
    return NULL;
}

/* ---- replies ---- */

typedef struct {
    int status;
    const char *code;
    const char *detail;
} verdict_reply_t;

static verdict_reply_t verdict_reply(radio_mux_verdict_t v)
{
    switch (v) {
    case RADIO_MUX_APPLY:
    case RADIO_MUX_APPLY_REBOOT:
        return (verdict_reply_t) {
            200, NULL, ""
        };
    case RADIO_MUX_MOVE:
        return (verdict_reply_t) {
            202, NULL, ""
        };
    case RADIO_MUX_REFUSE_BAD_VALUE:
        return (verdict_reply_t) {
            400, "bad_value", "a value is not accepted for this channel"
        };
    case RADIO_MUX_REFUSE_UNAVAILABLE:
        return (verdict_reply_t) {
            409, "unavailable", "the radio is not plugged in or has no driver"
        };
    case RADIO_MUX_REFUSE_NOT_AVAILABLE:
        return (verdict_reply_t) {
            409, "not_available", "this mode is not in this build"
        };
    case RADIO_MUX_REFUSE_INCOMPATIBLE:
        return (verdict_reply_t) {
            409, "incompatible", "the radio's firmware does not fit this mode"
        };
    case RADIO_MUX_REFUSE_MODE_TAKEN:
        return (verdict_reply_t) {
            409, "mode_taken", "another radio runs this mode"
        };
    case RADIO_MUX_REFUSE_HYBRID:
        return (verdict_reply_t) {
            409, "hybrid_not_supported", "Thread and Zigbee cannot run at once"
        };
    case RADIO_MUX_REFUSE_UNKNOWN_CHANNEL:
        return (verdict_reply_t) {
            404, "unknown_channel", "no radio with this id"
        };
    case RADIO_MUX_REFUSE_PROBE_FAILED:
        return (verdict_reply_t) {
            409, "probe_failed", "the radio did not answer as an OpenThread RCP"
        };
    case RADIO_MUX_REFUSE_BUSY:
        return (verdict_reply_t) {
            409, "busy", "the radio or OpenThread is busy, try again"
        };
    case RADIO_MUX_REFUSE_PENDING:
        return (verdict_reply_t) {
            503, "mux_busy", MUX_BUSY_DETAIL
        };
    case RADIO_MUX_REFUSE_FAILED:
    default:
        return (verdict_reply_t) {
            500, "failed", "the change could not be stored or applied"
        };
    }
}

void device_api_channel_refusal(radio_mux_verdict_t v, int *status, const char **code)
{
    const verdict_reply_t r = verdict_reply(v);

    *status = r.status;
    *code = r.code;
}

const char *device_api_channel_refusal_detail(radio_mux_verdict_t v)
{
    return verdict_reply(v).detail;
}

device_api_error_t device_api_channel_forget_error(esp_err_t err, bool mux_started)
{
    if (err == ESP_ERR_INVALID_STATE && !mux_started) {
        return (device_api_error_t) {
            503, "not_started", "the radio mux is not running"
        };
    }
    switch (err) {
    case ESP_OK:
        return (device_api_error_t) {
            204, NULL, NULL
        };
    case ESP_ERR_NOT_FOUND:
        return (device_api_error_t) {
            404, "unknown_channel", "no radio with this id"
        };
    case ESP_ERR_INVALID_STATE:
        return (device_api_error_t) {
            409, "in_use", "only an unplugged USB radio can be forgotten"
        };
    case ESP_ERR_NOT_FINISHED:
        return (device_api_error_t) {
            503, "mux_busy", MUX_BUSY_DETAIL
        };
    default:
        return (device_api_error_t) {
            500, "failed", "the radio could not be forgotten"
        };
    }
}

/* ---- decisions ---- */

static bool sets_otbr(const radio_mux_set_req_t *req)
{
    return req->has_mode && req->mode == RADIO_MUX_MODE_OTBR;
}

// otbr on a channel that does not have it: a move from the otbr channel, or a start when none has it.
static bool moves_to(const radio_mux_set_req_t *req, const char *id, const char *otbr_id)
{
    return sets_otbr(req) && (!otbr_id || strcmp(otbr_id, id) != 0);
}

bool device_api_channel_is_move(const radio_mux_set_req_t *req, const char *id, const char *otbr_id)
{
    const bool stop = req->has_mode && req->mode != RADIO_MUX_MODE_OTBR && otbr_id && strcmp(otbr_id, id) == 0;

    return moves_to(req, id, otbr_id) || stop;
}

const char *device_api_channel_move_refusal(const radio_mux_set_req_t *req, const char *id, const char *otbr_id)
{
    const bool more = req->has_baud || req->has_flow || req->has_proxy || req->kick;

    return more && device_api_channel_is_move(req, id, otbr_id)
           ? "send line or proxy settings in a separate request from starting, moving or stopping Thread" : NULL;
}

const char *device_api_channel_move_busy(const radio_mux_set_req_t *req, bool move_running)
{
    return req->has_mode && move_running ? "OpenThread is being moved to another radio" : NULL;
}

bool device_api_channel_retry_otbr(const radio_mux_set_req_t *req, radio_mux_verdict_t v, const char *id,
                                   const char *otbr_id, int mode)
{
    return v == RADIO_MUX_APPLY && sets_otbr(req) && otbr_id && strcmp(otbr_id, id) == 0 &&
           mode == BR_MODE_RADIO_FAULT;
}

void device_api_channels_overlay_board(radio_mux_snapshot_t *s, const device_api_board_view_t *b)
{
    if (!b->otbr_id) {
        return;
    }
    for (size_t i = 0; i < s->n && i < RADIO_MUX_CHANNELS_MAX; i++) {
        radio_mux_chan_info_t *c = &s->chans[i];

        if (strcmp(c->id, b->otbr_id) != 0 || c->state != RADIO_MUX_STATE_RUNNING) {
            continue; // only the OTBR channel the mux lists running: a C6 job's busy, a fault, a lease stay
        }
        if (b->mode == BR_MODE_RADIO_FAULT) {
            c->state = RADIO_MUX_STATE_FAULT;
            strlcpy(c->reason, b->fault_reason ? b->fault_reason : "", sizeof(c->reason));
        } else if (b->mode == BR_MODE_STARTING && !b->moving) {
            c->state = RADIO_MUX_STATE_STARTING; // attached, OpenThread not up yet (a move's target shows its own)
        } else if (b->refused_id && strcmp(b->refused_id, c->id) == 0 && c->reason[0] == '\0') {
            strlcpy(c->reason, b->refused_reason ? b->refused_reason : "", sizeof(c->reason));
        }
    }
}
