#pragma once

// GET /device/console/ws (console_ws.c): the Thread console's WebSocket transport.

#include "esp_err.h"
#include "esp_http_server.h"

/** Post-handshake: attach the socket to the console, or close it with 4003 when the console is off. */
esp_err_t device_api_console_ws_opened(httpd_req_t *req);

/** A frame from the client: text is a line for the CLI, a pong refreshes the socket. */
esp_err_t device_api_console_ws_frame(httpd_req_t *req);
