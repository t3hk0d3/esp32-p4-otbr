#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

/**
 * GET /device/diag (spec 5.12): the precomputed diag snapshot. Served in every mode; takes no OpenThread lock and waits at
 * most 100 ms for the snapshot mutex (503 "busy" then). 503 "not_started" before diag_start, 500 otherwise on failure.
 */
esp_err_t device_api_diag_get_handler(httpd_req_t *req);
