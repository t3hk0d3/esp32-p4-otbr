// /device/usbip body and PUT parser (spec docs/superpowers/specs/2026-11-10-usbip-design.md section 9). Pure.
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "device_api_priv.h"
#include "usbip_json.h"

static void key(json_out_t *w, const char *name)
{
    json_out_raw(w, ",\"");
    json_out_raw(w, name);
    json_out_raw(w, "\":");
}

static void counter(json_out_t *w, const char *name, uint32_t value)
{
    key(w, name);
    json_out_uint(w, value);
}

static void hex_str(json_out_t *w, unsigned value, int digits)
{
    char text[8];

    snprintf(text, sizeof(text), "%0*x", digits, value);
    json_out_str(w, text);
}

// An empty string is "not set": null.
static void str_or_null(json_out_t *w, const char *text)
{
    if (text[0]) {
        json_out_str(w, text);
    } else {
        json_out_raw(w, "null");
    }
}

static const char *speed_name(uint32_t speed)
{
    switch (speed) {
    case 1:
        return "low";
    case 2:
        return "full";
    case 3:
        return "high";
    default:
        return "unknown";
    }
}

static void write_device(json_out_t *w, const usbip_status_t *st)
{
    if (!st->has_device) {
        json_out_raw(w, "null");
        return;
    }
    json_out_raw(w, "{\"busid\":");
    json_out_str(w, st->dev.busid);
    key(w, "vid");
    hex_str(w, st->dev.vid, 4);
    key(w, "pid");
    hex_str(w, st->dev.pid, 4);
    key(w, "bcd_device");
    hex_str(w, st->dev.bcd_device, 4);
    key(w, "class");
    hex_str(w, st->dev.dev_class, 2);
    key(w, "speed");
    json_out_str(w, speed_name(st->dev.speed));
    key(w, "manufacturer");
    json_out_str(w, st->manufacturer);
    key(w, "product");
    json_out_str(w, st->product);
    key(w, "serial");
    json_out_str(w, st->serial);
    counter(w, "interfaces", st->dev.num_ifaces);
    counter(w, "endpoints", st->endpoints);
    json_out_raw(w, "}");
}

static void write_client(json_out_t *w, const usbip_status_t *st)
{
    if (!st->has_client) {
        json_out_raw(w, "null");
        return;
    }
    json_out_raw(w, "{\"addr\":");
    json_out_str(w, st->client);
    key(w, "since");
    json_out_u64(w, st->client_since > 0 ? (uint64_t)st->client_since : 0);
    json_out_raw(w, "}");
}

esp_err_t usbip_json_status(const usbip_status_t *st, char *buf, size_t cap, size_t *len)
{
    json_out_t w;

    json_out_init(&w, buf, cap);
    json_out_raw(&w, "{\"enabled\":");
    json_out_bool(&w, st->enabled);
    json_out_raw(&w, ",\"active\":");
    json_out_bool(&w, st->active);
    json_out_raw(&w, ",\"pending_reboot\":");
    json_out_bool(&w, st->pending_reboot);
    key(&w, "state");
    json_out_str(&w, usbip_state_name(st->state));
    key(&w, "reason");
    str_or_null(&w, st->reason);
    counter(&w, "port", st->port);
    key(&w, "interface");
    json_out_str(&w, st->iface ? st->iface : "single");
    key(&w, "device");
    write_device(&w, st);
    key(&w, "client");
    write_client(&w, st);
    json_out_raw(&w, ",\"counters\":{\"sessions\":");
    json_out_uint(&w, st->sessions);
    counter(&w, "refused", st->refused);
    counter(&w, "submits", st->counters.submits);
    counter(&w, "completed", st->counters.completed);
    counter(&w, "errors", st->counters.errors);
    counter(&w, "unlinks", st->counters.unlinks);
    key(&w, "bytes_in");
    json_out_u64(&w, st->counters.bytes_in);
    key(&w, "bytes_out");
    json_out_u64(&w, st->counters.bytes_out);
    counter(&w, "sessions_ended_overload", st->counters.overload_ends);
    json_out_raw(&w, "},\"memory\":{\"inflight_bytes\":");
    json_out_u64(&w, st->mem.inflight);
    key(&w, "inflight_peak");
    json_out_u64(&w, st->mem.inflight_peak);
    key(&w, "waiting_bytes");
    json_out_u64(&w, st->mem.waiting);
    key(&w, "waiting_peak");
    json_out_u64(&w, st->mem.waiting_peak);
    json_out_raw(&w, "}");
    key(&w, "last_error");
    str_or_null(&w, st->last_error);
    json_out_raw(&w, "}");
    *len = w.len;
    return w.overflow ? ESP_ERR_INVALID_SIZE : ESP_OK;
}

esp_err_t usbip_json_put_parse(const char *body, usbip_put_kind_t *kind, bool *enabled)
{
    esp_err_t err = ESP_ERR_INVALID_ARG;
    cJSON *root = body ? cJSON_Parse(body) : NULL;

    // Exactly one member. cJSON keeps duplicate keys as separate members, so a duplicate fails the count.
    if (!cJSON_IsObject(root) || cJSON_GetArraySize(root) != 1) {
        cJSON_Delete(root);
        return err;
    }
    const cJSON *item = root->child;

    if (strcmp(item->string, "enabled") == 0 && cJSON_IsBool(item)) {
        *kind = USBIP_PUT_SET;
        *enabled = cJSON_IsTrue(item);
        err = ESP_OK;
    } else if (strcmp(item->string, "kick") == 0 && cJSON_IsTrue(item)) {
        *kind = USBIP_PUT_KICK;
        err = ESP_OK;
    }
    cJSON_Delete(root);
    return err;
}
