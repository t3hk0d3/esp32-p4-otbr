// GET /device/diag: glue between esp_http_server and the diag snapshot (the JSON is built in diag).
#include "diag_handler.h"

#include <stdlib.h>

#include "diag.h"
#include "device_api_priv.h"

static esp_err_t send_error(httpd_req_t *req, int http_status, const char *code, const char *detail)
{
    char body[256];

    if (device_api_error_json(body, sizeof(body), code, detail) < 0) {
        device_api_error_json(body, sizeof(body), code, "");
    }
    httpd_resp_set_status(req, device_api_status_line(http_status));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

esp_err_t device_api_diag_get_handler(httpd_req_t *req)
{
    char *json = NULL;
    const esp_err_t err = diag_snapshot_json(&json);

    switch (err) {
    case ESP_OK:
        break;
    case ESP_ERR_TIMEOUT:
        return send_error(req, 503, "busy", "the diagnostics snapshot is being updated, retry");
    case ESP_ERR_INVALID_STATE:
        return send_error(req, 503, "not_started", "diagnostics are not running");
    case ESP_ERR_NO_MEM:
        return send_error(req, 500, "no_memory", "out of memory");
    default:
        return send_error(req, 500, "internal", "diagnostics snapshot failed");
    }
    httpd_resp_set_type(req, "application/json");
    const esp_err_t sent = httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);

    free(json);
    return sent;
}
