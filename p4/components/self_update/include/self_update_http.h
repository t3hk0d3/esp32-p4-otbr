#pragma once

#include "esp_err.h"
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

/** Returned by self_update_http_open when the redirect chain is longer than 5 hops. */
#define SELF_UPDATE_ERR_TOO_MANY_REDIRECTS ESP_ERR_INVALID_STATE

/**
 * Open `client` for a GET and fetch the response headers, following 301/302/303/307/308 by hand (open/fetch_headers
 * do not follow them, unlike esp_http_client_perform): the body of each redirect is drained and the client is
 * reopened at the new location, at most 5 hops.
 * ESP_OK: *status is the final status (the caller checks for 200); the response body is ready to read.
 * SELF_UPDATE_ERR_TOO_MANY_REDIRECTS: more than 5 hops. Any other esp_err: a transport or header failure.
 * The caller must esp_http_client_close() the client on every path (also after an error).
 */
esp_err_t self_update_http_open(esp_http_client_handle_t client, int *status);

#ifdef __cplusplus
}
#endif
