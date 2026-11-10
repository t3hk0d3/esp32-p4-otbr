#pragma once

// GET /device/logging/ws (logs_ws.c): the live log's WebSocket transport.

#include <stddef.h>
#include "esp_err.h"
#include "esp_http_server.h"

/** Live log clients connected now (each holds a logging viewer). Server task. */
size_t device_api_logs_ws_viewers(void);

/** Post-handshake: open a logging viewer at the URL's `level` (info by default) for the socket. */
esp_err_t device_api_logs_ws_opened(httpd_req_t *req);

/** A frame from the client: text {"level":"<name>"} changes its level, a pong refreshes the socket. */
esp_err_t device_api_logs_ws_frame(httpd_req_t *req);
