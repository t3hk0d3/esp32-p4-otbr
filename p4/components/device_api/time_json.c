// /device/time bodies (spec docs/superpowers/specs/2026-11-10-time-sync-design.md, section 5.2). Pure.
#include <string.h>

#include "cJSON.h"
#include "device_api_priv.h"
#include "time_json.h"

// A non-negative number, or null when `value` is below `min_valid`.
static void number_or_null(json_out_t *w, int64_t value, int64_t min_valid)
{
    if (value < min_valid) {
        json_out_raw(w, "null");
    } else {
        json_out_u64(w, (uint64_t)value);
    }
}

int device_api_time_json(char *buf, size_t cap, const timesync_status_t *st)
{
    json_out_t w;

    json_out_init(&w, buf, cap);
    json_out_raw(&w, "{\"enabled\":");
    json_out_bool(&w, st->config.enabled);
    json_out_raw(&w, ",\"mode\":");
    json_out_str(&w, timesync_mode_name(st->config.mode));
    json_out_raw(&w, ",\"server\":");
    json_out_str(&w, st->config.server);
    json_out_raw(&w, ",\"state\":");
    json_out_str(&w, timesync_state_name(st->state));
    json_out_raw(&w, ",\"now\":");
    number_or_null(&w, st->now, 1); // 0: the clock is not valid
    json_out_raw(&w, ",\"synced_age_s\":");
    number_or_null(&w, st->synced_age_s, 0); // -1: no answer since boot
    json_out_raw(&w, ",\"source\":");
    json_out_str(&w, timesync_source_name(st->servers.source));
    json_out_raw(&w, ",\"servers\":[");
    for (uint8_t i = 0; i < st->servers.count && i < TIMESYNC_MAX_SERVERS; i++) {
        json_out_raw(&w, i ? ",{\"address\":" : "{\"address\":");
        json_out_str(&w, st->servers.addr[i]);
        json_out_raw(&w, ",\"answered\":");
        json_out_bool(&w, st->answered == (int8_t)i);
        json_out_raw(&w, "}");
    }
    json_out_raw(&w, "],\"dhcp_servers\":[");
    for (uint8_t i = 0; i < st->dhcp.count && i < TIMESYNC_MAX_SERVERS; i++) {
        if (i) {
            json_out_raw(&w, ",");
        }
        json_out_str(&w, st->dhcp.addr[i]);
    }
    json_out_raw(&w, "],\"dhcp_iface\":");
    json_out_str(&w, timesync_iface_name(st->dhcp.iface));
    json_out_raw(&w, "}");
    return w.overflow ? -1 : (int)w.len;
}

esp_err_t device_api_time_parse(const char *body, timesync_config_t *out, const char **field)
{
    cJSON *json = cJSON_Parse(body);
    const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(json, "enabled");
    const cJSON *mode = cJSON_GetObjectItemCaseSensitive(json, "mode");
    const cJSON *server = cJSON_GetObjectItemCaseSensitive(json, "server");
    timesync_config_t c = {0};

    *field = NULL;
    if (!cJSON_IsObject(json)) {
        *field = "body";
    } else if (!cJSON_IsBool(enabled)) {
        *field = "enabled";
    } else if (!cJSON_IsString(mode) ||
               (strcmp(mode->valuestring, "auto") != 0 && strcmp(mode->valuestring, "custom") != 0)) {
        *field = "mode";
    } else if (!cJSON_IsString(server) || strlen(server->valuestring) >= sizeof(c.server)) {
        *field = "server";
    } else {
        c.enabled = cJSON_IsTrue(enabled);
        c.mode = strcmp(mode->valuestring, "custom") == 0 ? TIMESYNC_MODE_CUSTOM : TIMESYNC_MODE_AUTO;
        strlcpy(c.server, server->valuestring, sizeof(c.server));
    }
    cJSON_Delete(json);
    if (*field) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = c;
    return ESP_OK;
}
