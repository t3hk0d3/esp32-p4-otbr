#pragma once

#include "esp_http_client.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * HTTP client config for every URL the board fetches (OTA URL, radio firmware URL, versions.txt): http and https, https
 * verified against the certificate bundle, 2048 byte receive and 1024 byte send buffers (a signed redirect Location
 * header does not fit the default 512). `url` must stay valid while the config is used.
 */
esp_http_client_config_t self_update_http_config(const char *url, int timeout_ms);

#ifdef __cplusplus
}
#endif
