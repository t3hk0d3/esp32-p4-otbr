// /device/usb_serial bodies (spec docs/superpowers/specs/2026-11-05-usb-serial-proxy-design.md). Pure.
#include "usb_serial_json.h"

#include <stdio.h>

#include "cJSON.h"
#include "device_api_priv.h"

static void hex4(json_out_t *w, uint16_t v)
{
    char tmp[8];

    snprintf(tmp, sizeof(tmp), "%04x", v);
    json_out_str(w, tmp);
}

int device_api_usb_serial_json(char *out, size_t cap, const usb_serial_settings_t *s, const usb_serial_status_t *st)
{
    json_out_t w;

    json_out_init(&w, out, cap);
    json_out_raw(&w, "{\"enabled\":");
    json_out_bool(&w, s->enabled);
    json_out_raw(&w, ",\"port\":");
    json_out_uint(&w, s->port);
    json_out_raw(&w, ",\"baud\":");
    json_out_uint(&w, s->baud);
    json_out_raw(&w, ",\"flow_control\":");
    json_out_bool(&w, s->flow_control);
    json_out_raw(&w, ",\"dongle\":");
    if (st->dongle.present) {
        json_out_raw(&w, "{\"vid\":");
        hex4(&w, st->dongle.vid);
        json_out_raw(&w, ",\"pid\":");
        hex4(&w, st->dongle.pid);
        json_out_raw(&w, ",\"driver\":");
        json_out_str(&w, usb_serial_driver_name(st->dongle.driver)); // NULL writes null
        json_out_raw(&w, ",\"open\":");
        json_out_bool(&w, st->dongle.open);
        json_out_raw(&w, ",\"flow_control\":");
        json_out_bool(&w, st->dongle.flow_on);
        json_out_raw(&w, ",\"error\":");
        json_out_str(&w, st->dongle.error[0] ? st->dongle.error : NULL);
        json_out_raw(&w, "}");
    } else {
        json_out_raw(&w, "null");
    }
    json_out_raw(&w, ",\"client\":");
    if (st->client.present) {
        json_out_raw(&w, "{\"peer\":");
        json_out_str(&w, st->client.peer);
        json_out_raw(&w, ",\"since_s\":");
        json_out_uint(&w, st->client.since_s);
        json_out_raw(&w, "}");
    } else {
        json_out_raw(&w, "null");
    }
    json_out_raw(&w, ",\"bytes_to_dongle\":");
    json_out_u64(&w, st->bytes_to_dongle);
    json_out_raw(&w, ",\"bytes_from_dongle\":");
    json_out_u64(&w, st->bytes_from_dongle);
    json_out_raw(&w, ",\"discarded_from_client\":");
    json_out_u64(&w, st->discarded_from_client);
    json_out_raw(&w, ",\"overflow_disconnects\":");
    json_out_uint(&w, st->overflow_disconnects);
    json_out_raw(&w, ",\"refused_connections\":");
    json_out_uint(&w, st->refused_connections);
    json_out_raw(&w, "}");
    return w.overflow ? -1 : (int)w.len;
}

// An integer in 0..max, else 0 (refused by usb_serial_settings_check).
static uint32_t whole(const cJSON *v, double max)
{
    const double d = v->valuedouble;

    return (d >= 0 && d <= max && (double)(uint32_t)d == d) ? (uint32_t)d : 0;
}

esp_err_t device_api_usb_serial_parse(const char *body, usb_serial_settings_t *inout, bool *kick)
{
    cJSON *json = cJSON_Parse(body);

    if (!cJSON_IsObject(json)) {
        cJSON_Delete(json);
        return ESP_ERR_INVALID_ARG;
    }
    const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(json, "enabled");
    const cJSON *port = cJSON_GetObjectItemCaseSensitive(json, "port");
    const cJSON *baud = cJSON_GetObjectItemCaseSensitive(json, "baud");
    const cJSON *flow = cJSON_GetObjectItemCaseSensitive(json, "flow_control");
    const cJSON *k = cJSON_GetObjectItemCaseSensitive(json, "kick");
    const bool ok = (!enabled || cJSON_IsBool(enabled)) && (!port || cJSON_IsNumber(port)) &&
                    (!baud || cJSON_IsNumber(baud)) && (!flow || cJSON_IsBool(flow)) && (!k || cJSON_IsBool(k));

    if (ok) {
        if (enabled) {
            inout->enabled = cJSON_IsTrue(enabled);
        }
        if (port) {
            inout->port = (uint16_t)whole(port, UINT16_MAX);
        }
        if (baud) {
            inout->baud = whole(baud, UINT32_MAX);
        }
        if (flow) {
            inout->flow_control = cJSON_IsTrue(flow);
        }
        *kick = k && cJSON_IsTrue(k);
    }
    cJSON_Delete(json);
    return ok ? ESP_OK : ESP_ERR_INVALID_ARG;
}
