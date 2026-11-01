/*
 * HTTP upload entry of the paired OTA (patch 5): feeds a request body into the esp_br_ota_* core.
 */

#include <stdlib.h>
#include "esp_br_http_ota.h"
#include "esp_check.h"
#include "esp_log.h"

#define TAG "BR_OTA"
#define UPLOAD_CHUNK_SIZE 4096
#define UPLOAD_MAX_TIMEOUTS 5 /* consecutive receive timeouts before the upload is given up */

esp_err_t esp_br_ota_from_httpd(httpd_req_t *req)
{
    ESP_RETURN_ON_FALSE(req != NULL, ESP_ERR_INVALID_ARG, TAG, "req is NULL");
    ESP_RETURN_ON_FALSE(req->content_len > 0, ESP_ERR_INVALID_SIZE, TAG, "upload needs a Content-Length");

    esp_br_ota_handle_t ota = NULL;
    ESP_RETURN_ON_ERROR(esp_br_ota_begin(&ota), TAG, "Failed to begin OTA");
    char *buf = malloc(UPLOAD_CHUNK_SIZE);
    if (!buf) {
        esp_br_ota_abort(ota);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = ESP_OK;
    size_t remaining = req->content_len;
    int timeouts = 0;
    while (remaining > 0 && err == ESP_OK) {
        int got = httpd_req_recv(req, buf, remaining < UPLOAD_CHUNK_SIZE ? remaining : UPLOAD_CHUNK_SIZE);
        if (got == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts <= UPLOAD_MAX_TIMEOUTS) {
            continue;
        }
        if (got <= 0) {
            ESP_LOGE(TAG, "upload interrupted (%d), %u bytes missing", got, (unsigned)remaining);
            err = ESP_FAIL;
            break;
        }
        timeouts = 0;
        remaining -= got;
        err = esp_br_ota_write(ota, buf, got);
    }
    free(buf);
    if (err != ESP_OK) {
        esp_br_ota_abort(ota);
        return err;
    }
    return esp_br_ota_end(ota); /* releases the update on every path */
}
