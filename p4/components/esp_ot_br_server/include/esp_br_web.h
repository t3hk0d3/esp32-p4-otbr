/*
 * SPDX-FileCopyrightText: 2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_event.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/** The web server's max_open_sockets (patch 21): the lwIP socket budget of the routing task counts it. */
#define ESP_BR_WEB_MAX_OPEN_SOCKETS 7

ESP_EVENT_DECLARE_BASE(ESP_BR_WEB_EVENT);

typedef enum {
    ESP_BR_WEB_EVENT_SERVER_STARTED, /*!< Border router web server is ready to accept connections */
} esp_br_web_event_t;

/**
 * @brief Check whether the border router web server has started
 *
 * @return true when the web server is ready to accept connections
 */
bool esp_br_web_is_server_started(void);

/**
 * @brief Tell the server whether OpenThread is fully initialised (patch 9). Until it is, the Thread REST API
 * (/node, /api, ...) answers 503. Set by br_core.
 */
void esp_br_web_set_openthread_ready(bool ready);

/** @brief True once esp_br_web_set_openthread_ready(true) was called. */
bool esp_br_web_openthread_ready(void);

/**
 * @brief Handle of the running HTTP server (patch 1), for registering more URI handlers.
 *
 * @return the handle, or NULL until the server has started
 */
httpd_handle_t esp_br_web_get_server(void);

/**
 * @brief Set the callback that registers more URI handlers (patch 10). It runs while the server starts, before the
 * catch-all wildcard GET handler is registered (a handler registered after it would never see a GET). Call it before
 * esp_br_web_start. Used by device_api.
 */
void esp_br_web_set_extra_handlers(esp_err_t (*cb)(httpd_handle_t server));

/**
 * @brief Start border router web server, which provides REST APIs and GUI
 *
 * @param[in] base_path is the virtual file path of web server
 */
void esp_br_web_start(char *base_path);

/** Size of an ePSKc TAP string buffer: 8 digits + checksum digit + '\0' */
#define ESP_BR_EPSKC_TAP_LEN 10

/**
 * @brief Record (or clear) the currently active ePSKc TAP, so that any UI which
 * did not itself activate the ephemeral key (e.g. the M5Stack on-screen UI when
 * the REST API started the session, or vice versa) can still discover and
 * display it. Backed by an atomic, independently thread-safe of the OpenThread
 * lock, so it is safe to call regardless of whether that lock is held.
 *
 * @param[in] tap The active TAP string, or NULL/empty to clear it (session stopped).
 */
void esp_br_web_epskc_set_active_tap(const char *tap);

/**
 * @brief Get the currently known active ePSKc TAP, if any. Backed by an atomic,
 * independently thread-safe of the OpenThread lock, so it is safe to call
 * regardless of whether that lock is held.
 *
 * @param[out] tap_out Buffer to receive the TAP string, at least ESP_BR_EPSKC_TAP_LEN bytes.
 * @param[in] tap_out_len Size of tap_out.
 * @return true if a TAP is currently known and was copied into tap_out, false otherwise.
 */
bool esp_br_web_epskc_get_active_tap(char *tap_out, size_t tap_out_len);

#ifdef __cplusplus
}
#endif
