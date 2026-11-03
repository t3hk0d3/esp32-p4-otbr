// /device/names bodies (spec docs/superpowers/specs/2026-11-03-network-names-design.md, section 6). Pure.
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "device_api_priv.h"
#include "names_json.h"

int device_api_names_json(char *buf, size_t cap, const netnames_t *n, bool split, const netnames_result_t *res)
{
    json_out_t w;

    json_out_init(&w, buf, cap);
    json_out_raw(&w, "{\"hostname\":");
    json_out_str(&w, n->set.hostname);
    json_out_raw(&w, ",\"hostname_default\":");
    json_out_str(&w, n->hostname_default);
    json_out_raw(&w, ",\"hostname_live\":");
    json_out_str(&w, n->hostname_live);
    json_out_raw(&w, ",\"ha_name_follows_hostname\":");
    json_out_bool(&w, n->set.ha_name_follows_hostname);
    json_out_raw(&w, ",\"ha_name\":");
    json_out_str(&w, n->set.ha_name);
    json_out_raw(&w, ",\"ha_name_effective\":");
    json_out_str(&w, n->ha_name);
    json_out_raw(&w, ",\"http_advert\":");
    json_out_bool(&w, n->set.http_advert);
    json_out_raw(&w, ",\"http_advert_available\":");
    json_out_bool(&w, n->http_advert_available);
    json_out_raw(&w, ",\"dhcp_name\":");
    json_out_str(&w, n->set.dhcp_name);
    json_out_raw(&w, ",\"dhcp_name_default\":");
    json_out_str(&w, NETNAMES_DEFAULT_DHCP_NAME);
    json_out_raw(&w, ",\"mode\":");
    json_out_str(&w, split ? "split" : "single");
    if (res) {
        json_out_raw(&w, ",\"applied\":");
        json_out_bool(&w, res->applied);
        if (res->detail[0]) {
            json_out_raw(&w, ",\"detail\":");
            json_out_str(&w, res->detail);
        }
    }
    json_out_raw(&w, "}");
    return w.overflow ? -1 : (int)w.len;
}

int device_api_names_error_json(char *buf, size_t cap, const char *field, const char *reason)
{
    char detail[128];
    json_out_t w;

    snprintf(detail, sizeof(detail), "%s: %s", field, reason);
    json_out_init(&w, buf, cap);
    json_out_raw(&w, "{\"error\":\"invalid_config\",\"field\":");
    json_out_str(&w, field);
    json_out_raw(&w, ",\"detail\":");
    json_out_str(&w, detail);
    json_out_raw(&w, "}");
    return w.overflow ? -1 : (int)w.len;
}

static bool get_str(const cJSON *json, const char *key, char out[NETNAMES_NAME_LEN])
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(json, key);

    if (!cJSON_IsString(item) || strlen(item->valuestring) >= NETNAMES_NAME_LEN) {
        return false;
    }
    strlcpy(out, item->valuestring, NETNAMES_NAME_LEN);
    return true;
}

static bool get_bool(const cJSON *json, const char *key, bool *out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(json, key);

    if (!cJSON_IsBool(item)) {
        return false;
    }
    *out = cJSON_IsTrue(item);
    return true;
}

esp_err_t device_api_names_parse(const char *body, netnames_settings_t *out, const char **field)
{
    cJSON *json = cJSON_ParseWithOpts(body, NULL, true); // like netcfg_from_json: nothing may follow the object
    netnames_settings_t s = {0};

    *field = NULL;
    if (!cJSON_IsObject(json)) {
        *field = "body";
    } else if (!get_str(json, "hostname", s.hostname)) {
        *field = "hostname";
    } else if (!get_bool(json, "ha_name_follows_hostname", &s.ha_name_follows_hostname)) {
        *field = "ha_name_follows_hostname";
    } else if (!get_str(json, "ha_name", s.ha_name)) {
        *field = "ha_name";
    } else if (!get_bool(json, "http_advert", &s.http_advert)) {
        *field = "http_advert";
    } else if (!get_str(json, "dhcp_name", s.dhcp_name)) {
        *field = "dhcp_name";
    }
    cJSON_Delete(json);
    if (*field) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = s;
    return ESP_OK;
}
