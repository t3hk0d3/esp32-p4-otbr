// /device/console bodies (spec docs/superpowers/specs/2026-11-04-thread-console-design.md). Pure.
#include "console_json.h"

#include "cJSON.h"
#include "device_api_priv.h"

int device_api_console_json(char *out, size_t cap, bool enabled, const char *peer, uint32_t since_s)
{
    json_out_t w;

    json_out_init(&w, out, cap);
    json_out_raw(&w, "{\"enabled\":");
    json_out_bool(&w, enabled);
    json_out_raw(&w, ",\"session\":");
    if (peer) {
        json_out_raw(&w, "{\"peer\":");
        json_out_str(&w, peer);
        json_out_raw(&w, ",\"since_s\":");
        json_out_uint(&w, since_s);
        json_out_raw(&w, "}");
    } else {
        json_out_raw(&w, "null");
    }
    json_out_raw(&w, "}");
    return w.overflow ? -1 : (int)w.len;
}

esp_err_t device_api_console_parse(const char *body, bool *enabled)
{
    cJSON *json = cJSON_Parse(body);
    const cJSON *value = cJSON_IsObject(json) ? cJSON_GetObjectItemCaseSensitive(json, "enabled") : NULL;
    const bool ok = cJSON_IsBool(value);

    if (ok) {
        *enabled = cJSON_IsTrue(value);
    }
    cJSON_Delete(json);
    return ok ? ESP_OK : ESP_ERR_INVALID_ARG;
}
