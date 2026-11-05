#include "self_update_http.h"

#include "esp_crt_bundle.h"

#define HTTP_RX_BUFFER_SIZE 2048
#define HTTP_TX_BUFFER_SIZE 1024

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
