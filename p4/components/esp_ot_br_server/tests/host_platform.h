/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include "cJSON.h"
#include <arpa/inet.h>
#include <assert.h>
#include <ctype.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <openthread/border_agent.h>
#include <openthread/border_agent_ephemeral_key.h>
#include <openthread/commissioner.h>
#include <openthread/dataset.h>
#include <openthread/dataset_ftd.h>
#include <openthread/instance.h>
#include <openthread/ip6.h>
#include <openthread/link.h>
#include <openthread/mesh_diag.h>
#include <openthread/netdiag.h>
#include <openthread/thread.h>
#include <openthread/thread_ftd.h>
#include <openthread/platform/radio.h>
#include "br_core_mode.h"
#include "esp_br_guard.h"

#define ESP_RETURN_ON_FALSE(cond, err, tag, ...) \
    do {                                         \
        if (!(cond)) {                           \
            return err;                          \
        }                                        \
    } while (0)
#define ESP_GOTO_ON_FALSE(cond, err, label, tag, ...) \
    do {                                              \
        if (!(cond)) {                                \
            ret = err;                                \
            goto label;                               \
        }                                             \
    } while (0)
#define ESP_ERR_INVALID_ARG -3
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM -2
#define portMAX_DELAY UINT32_MAX
#define pdTRUE 1
#define pdPASS 1
#define pdMS_TO_TICKS(ms) (ms)
typedef int esp_err_t;
#define API_TAG "web_api"
#define BASE_TAG "web_base"
#define OPENTHREAD_INVALID_RLOC16 "0xffff"
typedef void *TaskHandle_t;
typedef struct {
    bool ready;
} * SemaphoreHandle_t;
typedef void *httpd_handle_t;
typedef enum { HTTP_GET, HTTP_POST, HTTP_PUT, HTTP_DELETE, HTTP_OPTIONS, HTTP_HEAD, HTTP_PATCH } httpd_method_t;
typedef struct {
    httpd_method_t method;
    const char *uri;
    size_t content_len;
    const char *input;
    size_t received;
    size_t fragment;
    cJSON *headers;
    const char *borrowed_status;
    const char *borrowed_type;
    const char *borrowed_keys[16];
    const char *borrowed_values[16];
    unsigned header_count;
    char *status;
    char *type;
    char *body;
    cJSON *sent_headers;
} httpd_req_t;
typedef struct {
    const char *uri;
    httpd_method_t method;
    esp_err_t (*handler)(httpd_req_t *);
} httpd_uri_t;

extern otDeviceRole mock_role;
extern otCommissionerState mock_commissioner;
extern otError mock_dataset_error, mock_pending_error, mock_pending_result;
extern int64_t mock_time;
extern otOperationalDataset mock_dataset;
extern otOperationalDatasetTlvs mock_active_tlvs; /* stored active TLVs: read by otDatasetGetActiveTlvs, set by otDatasetSetActiveTlvs */
extern otOperationalDatasetTlvs mock_update_tlvs; /* otDatasetUpdateTlvs result when mLength != 0 (else the base is unchanged) */
extern otOperationalDatasetTlvs mock_convert_tlvs; /* otDatasetConvertToTlvs result when mLength != 0 */
extern otError mock_set_active_error, mock_thread_enable_error, mock_clear_pending_error; /* injected failures */
extern char mock_calls[32]; /* order of Thread/dataset writes: 'D' disable, 'P' delete pending, 'S' store active, 'E' enable */
void mock_call(char call);
extern unsigned mock_registered;
extern br_core_mode_t mock_mode;
extern bool mock_ot_ready;
extern unsigned mock_ot_writes; /* calls that change OpenThread state */
extern unsigned mock_lock_acquires; /* esp_openthread_lock_acquire() calls so far */
extern void (*mock_on_lock)(void); /* runs once, right after the next lock acquire */
br_core_mode_t br_core_get_mode(void);
extern bool mock_session_refused; /* br_core_ot_session_begin() refuses (a radio push is stopping OpenThread) */
extern int mock_sessions_open;    /* sessions opened and not yet closed */
bool br_core_ot_session_begin(void);
void br_core_ot_session_end(void);
bool esp_br_web_openthread_ready(void);
void mock_reset(void);
#ifndef ESP_LOGI
#define ESP_LOGI(tag, ...) ((void)0) /* logging is not checked on the host */
#define ESP_LOGW(tag, ...) ((void)0)
#endif
void mock_deliver_diagnostic(uintptr_t token);
/* netdiag_mux (patch 17): the host build forwards to the mocked otThreadSendDiagnosticGet, which captures the callback. */
otError netdiag_mux_send_get(const otIp6Address *dest, const uint8_t *tlvs, uint8_t count,
                             otReceiveDiagnosticGetCallback cb, void *ctx, bool filter_peer);
extern unsigned mock_netdiag_sends;           /* netdiag_mux_send_get() calls so far */
extern unsigned mock_netdiag_filtered_sends;  /* of those, with filter_peer set */
void netdiag_mux_release(otReceiveDiagnosticGetCallback cb);
extern unsigned mock_netdiag_releases;        /* netdiag_mux_release() calls so far (patch 23) */
otInstance *esp_openthread_get_instance(void);
bool esp_openthread_lock_acquire(uint32_t timeout);
void esp_openthread_lock_release(void);
void esp_fill_random(void *buffer, size_t length);
int64_t esp_timer_get_time(void);
void esp_br_web_epskc_set_active_tap(const char *tap);
void esp_restart(void);
void reboot_gate_restart(uint32_t delay_ms); /* patch 23 */
SemaphoreHandle_t xSemaphoreCreateBinary(void);
int xSemaphoreTake(SemaphoreHandle_t semaphore, uint32_t timeout);
int xSemaphoreGive(SemaphoreHandle_t semaphore);
int xTaskCreate(void (*task)(void *), const char *name, unsigned stack, void *arg, unsigned priority,
                TaskHandle_t *handle);
int xTaskCreatePinnedToCore(void (*task)(void *), const char *name, unsigned stack, void *arg, unsigned priority,
                            TaskHandle_t *handle, int core);
/* netcfg (patch 12): the per-interface policy; the mock allows everything unless a test sets mock_netcfg_deny. */
typedef enum { NETCFG_SVC_GUI, NETCFG_SVC_DEVICE_API, NETCFG_SVC_REST_READ, NETCFG_SVC_REST_WRITE } netcfg_service_t;
extern bool mock_netcfg_deny;
extern netcfg_service_t mock_netcfg_last_service;
bool netcfg_request_allowed(httpd_req_t *req, netcfg_service_t svc);
void vTaskDelay(uint32_t ticks);
/* The /topology collector (patch 17): time advances with vTaskDelay; mock_on_delay runs at each delay. */
typedef uint32_t TickType_t;
TickType_t xTaskGetTickCount(void);
uint32_t esp_get_free_heap_size(void);
extern TickType_t mock_ticks;
extern void (*mock_on_delay)(void);
/* Scripted otThreadGetNextDiagnosticTlv answer: the TLVs the next iteration over a message returns. */
extern otNetworkDiagTlv mock_diag_tlvs[8];
extern unsigned mock_diag_tlv_count;
size_t strlcpy(char *dest, const char *source, size_t length);
size_t httpd_req_get_hdr_value_len(httpd_req_t *req, const char *key);
int httpd_req_get_hdr_value_str(httpd_req_t *req, const char *key, char *value, size_t capacity);
int httpd_req_recv(httpd_req_t *req, char *buffer, size_t length);
int httpd_resp_set_status(httpd_req_t *req, const char *status);
int httpd_resp_set_type(httpd_req_t *req, const char *type);
int httpd_resp_set_hdr(httpd_req_t *req, const char *key, const char *value);
int httpd_resp_send(httpd_req_t *req, const char *body, size_t length);
int httpd_register_uri_handler(httpd_handle_t server, const httpd_uri_t *route);
int hex_to_string(const uint8_t *bytes, char *text, size_t size);
