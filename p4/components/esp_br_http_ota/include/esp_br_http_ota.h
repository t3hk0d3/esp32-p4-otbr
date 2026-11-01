/*
 * SPDX-FileCopyrightText: 2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_http_client.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Handle of the one update that may run at a time. */
typedef struct esp_br_ota *esp_br_ota_handle_t;

/**
 * @brief Start a paired update (spec 5.6). Targets the inactive pair web_M / ota_M, never the running slot.
 *
 * @return ESP_OK; ESP_ERR_INVALID_STATE if an update is in progress or an earlier one is committed and waits for its
 *         reboot; ESP_ERR_NOT_FOUND / ESP_ERR_NOT_SUPPORTED if the inactive pair cannot be determined.
 */
esp_err_t esp_br_ota_begin(esp_br_ota_handle_t *out);

/**
 * @brief Feed the next bytes of a bundle (any chunking). Bundle header, then web image (written raw to web_M, erased
 *        first, SHA-256 compared against the header once complete), then the plain P4 image (esp_ota_* into ota_M).
 *        After an error call esp_br_ota_abort().
 */
esp_err_t esp_br_ota_write(esp_br_ota_handle_t h, const void *data, size_t len);

/**
 * @brief End of the bundle: validates the P4 image (esp_ota_end) and, only then, sets ota_M as boot partition.
 *        The update is released on every path; do not call esp_br_ota_abort() afterwards. ESP_ERR_INVALID_SIZE if the
 *        bundle was truncated.
 */
esp_err_t esp_br_ota_end(esp_br_ota_handle_t h);

/** @brief Give up an update: releases the ota handle and the update slot; the boot partition is not changed. */
void esp_br_ota_abort(esp_br_ota_handle_t h);

/**
 * @brief Upload entry: feeds the request body (Content-Length required) in chunks of up to 4 KB through the core.
 *        Does not send a response; the caller replies (and reboots after success).
 */
esp_err_t esp_br_ota_from_httpd(httpd_req_t *req);

#ifdef OTBR_TEST_BUILD
/** Test builds only: wait this long before the boot partition is set (to reset the board before the commit). */
void esp_br_ota_test_set_commit_delay_ms(uint32_t ms);
#endif

/**
 * @brief Border Router OTA by URL: stream the HTTP(S) response body (a bundle) through the esp_br_ota_* core.
 *        The RCP firmware is not part of the bundle (R14).
 *
 * @param[in] http_config       The HTTP server download config
 *
 * @return
 *  - ESP_OK
 *  - ESP_FAIL                  Download failed; the boot partition is unchanged.
 *  - ESP_ERR_INVALID_STATE     Another update is in progress.
 *  - ESP_ERR_INVALID_ARG       If the http config is NULL or does not contain an url.
 *
 */
esp_err_t esp_br_http_ota(esp_http_client_config_t *http_config);

#define OTA_MAX_WRITE_SIZE 16

#ifdef __cplusplus
} /* extern "C" */
#endif
