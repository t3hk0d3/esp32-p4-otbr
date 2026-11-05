#include "self_update_http.h"

#include <stdio.h>
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "self_update.h"

#define HTTP_RX_BUFFER_SIZE 2048
#define HTTP_TX_BUFFER_SIZE 1024

static const char *TAG = "self_update_http";

esp_http_client_config_t self_update_http_config(const char *url, int timeout_ms)
{
    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = timeout_ms,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = HTTP_RX_BUFFER_SIZE,
        .buffer_size_tx = HTTP_TX_BUFFER_SIZE,
    };

    return config;
}

// Every detail names the URL asked for, never a redirect target.
static esp_err_t fetch_versions(const char *url, char *buf, size_t cap, size_t *len, char *detail, size_t detail_cap)
{
    const esp_http_client_config_t config = self_update_http_config(url, SELF_UPDATE_FETCH_TIMEOUT_MS);
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_err_t err;

    if (!client) {
        snprintf(detail, detail_cap, "out of memory (%s)", url);
        return ESP_ERR_NO_MEM;
    }
    err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        snprintf(detail, detail_cap, "%s (%s)", esp_err_to_name(err), url);
        goto done;
    }
    esp_http_client_fetch_headers(client);
    const int status = esp_http_client_get_status_code(client);

    if (status != 200) {
        snprintf(detail, detail_cap, "HTTP %d (%s)", status, url);
        err = ESP_ERR_NOT_FOUND;
        goto close;
    }
    // At most SELF_UPDATE_VERSIONS_MAX bytes are valid; one more is read to tell "exactly the limit" from "more".
    size_t got = 0;

    while (got < cap) {
        const int n = esp_http_client_read(client, buf + got, (int)(cap - got));

        if (n < 0) {
            snprintf(detail, detail_cap, "read failed (%s)", url);
            err = ESP_FAIL;
            goto close;
        }
        if (n == 0) {
            break;
        }
        got += (size_t)n;
    }
    if (got > SELF_UPDATE_VERSIONS_MAX) {
        snprintf(detail, detail_cap, "versions.txt too large (%s)", url);
        err = ESP_ERR_INVALID_SIZE;
        goto close;
    }
    *len = got;
close:
    esp_http_client_close(client);
done:
    esp_http_client_cleanup(client);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s", detail);
    }
    return err;
}

static int64_t now_us(void)
{
    return esp_timer_get_time();
}

const self_update_ops_t self_update_http_ops = {
    .fetch = fetch_versions,
    .now_us = now_us,
};
