// /device/thread bodies (spec docs/superpowers/specs/2026-11-03-leader-weight-design.md, section 3). Pure.
#include <stdio.h>

#include "cJSON.h"
#include "device_api_priv.h"
#include "thread_json.h"

#define KEY_ADJUSTMENT "leader_weight_adjustment"
#define KEY_BECOME "become_leader"

int device_api_thread_json(char *buf, size_t cap, const device_api_thread_t *t)
{
    json_out_t w;

    json_out_init(&w, buf, cap);
    json_out_raw(&w, "{\"" KEY_ADJUSTMENT "\":");
    json_out_int(&w, t->adjustment);
    if (t->weight_known) {
        json_out_raw(&w, ",\"leader_weight\":");
        json_out_uint(&w, t->weight);
        json_out_raw(&w, ",\"leader_weight_base\":");
        json_out_int(&w, (int32_t)t->weight - t->adjustment);
    } else if (t->partial) {
        json_out_raw(&w, ",\"partial\":true");
    }
    if (t->has_applied) {
        json_out_raw(&w, ",\"applied\":");
        json_out_bool(&w, t->applied);
        if (t->detail && t->detail[0]) {
            json_out_raw(&w, ",\"detail\":");
            json_out_str(&w, t->detail);
        }
    }
    if (t->become_started) {
        json_out_raw(&w, ",\"" KEY_BECOME "\":\"started\"");
    }
    json_out_raw(&w, "}");
    return w.overflow ? -1 : (int)w.len;
}

int device_api_thread_error_json(char *buf, size_t cap, const char *code, const char *detail, const device_api_thread_t *t)
{
    json_out_t w;

    json_out_init(&w, buf, cap);
    json_out_raw(&w, "{\"error\":");
    json_out_str(&w, code);
    json_out_raw(&w, ",\"detail\":");
    json_out_str(&w, detail ? detail : "");
    if (t && t->has_applied) {
        json_out_raw(&w, ",\"" KEY_ADJUSTMENT "\":");
        json_out_int(&w, t->adjustment);
        json_out_raw(&w, ",\"applied\":");
        json_out_bool(&w, t->applied);
    }
    json_out_raw(&w, "}");
    return w.overflow ? -1 : (int)w.len;
}

void device_api_thread_refusal(br_core_leader_refusal_t refusal, int mode, int *status, const char **code, char *detail,
                               size_t cap)
{
    *status = 409;
    *code = "cannot_take_over";
    switch (refusal) {
    case BR_CORE_LEADER_NOT_RUNNING:
        *code = "busy";
        snprintf(detail, cap, "mode %s", device_api_mode_name(mode));
        return;
    case BR_CORE_LEADER_NOT_ATTACHED:
        snprintf(detail, cap, "not attached");
        return;
    case BR_CORE_LEADER_ALREADY:
        snprintf(detail, cap, "already leader");
        return;
    case BR_CORE_LEADER_NOT_ELIGIBLE:
        snprintf(detail, cap, "not router eligible");
        return;
    case BR_CORE_LEADER_WEIGHT_NOT_HIGHER:
    case BR_CORE_LEADER_OK: // not a refusal; never passed here
    default:
        snprintf(detail, cap, "weight not higher");
        return;
    }
}

esp_err_t device_api_thread_parse(const char *body, device_api_thread_put_t *out, const char **field)
{
    cJSON *json = cJSON_Parse(body);
    device_api_thread_put_t put = {0};

    *field = NULL;
    if (!cJSON_IsObject(json)) {
        *field = "body";
    } else {
        const cJSON *adj = cJSON_GetObjectItemCaseSensitive(json, KEY_ADJUSTMENT);
        const cJSON *become = cJSON_GetObjectItemCaseSensitive(json, KEY_BECOME);

        if (adj) {
            // cJSON keeps one number type: accept any whole value, so 8.0 is 8 and 8.5 is not.
            // The range comes first on the double, so the cast to int is safe.
            if (!cJSON_IsNumber(adj) || !(adj->valuedouble >= BR_CORE_LEADER_ADJ_MIN && adj->valuedouble <= BR_CORE_LEADER_ADJ_MAX) ||
                    (double)(int)adj->valuedouble != adj->valuedouble) {
                *field = KEY_ADJUSTMENT;
            } else {
                put.has_adjustment = true;
                put.adjustment = (int8_t)adj->valuedouble;
            }
        }
        if (!*field && become) {
            if (!cJSON_IsBool(become)) {
                *field = KEY_BECOME;
            } else {
                put.become_leader = cJSON_IsTrue(become);
            }
        }
        if (!*field && !adj && !become) {
            *field = "body";
        }
    }
    cJSON_Delete(json);
    if (*field) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = put;
    return ESP_OK;
}
