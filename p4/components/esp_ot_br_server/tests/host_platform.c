/* SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0 */
#include "host_platform.h"
unsigned mock_registered;
br_core_mode_t mock_mode = BR_MODE_BR_RUNNING;
bool mock_ot_ready = true;
unsigned mock_ot_writes;
void (*mock_on_lock)(void);
unsigned mock_lock_acquires;
bool mock_session_refused;
int mock_sessions_open;
br_core_mode_t br_core_get_mode(void)
{
    return mock_mode;
}
bool br_core_ot_session_begin(void)
{
    if (mock_session_refused) {
        return false;
    }
    mock_sessions_open++;
    return true;
}
void br_core_ot_session_end(void)
{
    assert(mock_sessions_open > 0);
    mock_sessions_open--;
}
bool esp_br_web_openthread_ready(void)
{
    return mock_ot_ready;
}
static unsigned random_counter;
static struct {
    bool ready;
} semaphore_storage;
int64_t mock_time;
void esp_fill_random(void *buffer, size_t length)
{
    unsigned char *p = buffer;
    while (length--) {
        *p++ = (unsigned char)++random_counter;
    }
}
int64_t esp_timer_get_time(void)
{
    return mock_time;
}
otInstance *esp_openthread_get_instance(void)
{
    return (otInstance *)(uintptr_t)1;
}
unsigned mock_netdiag_sends;
unsigned mock_netdiag_filtered_sends;
otError netdiag_mux_send_get(const otIp6Address *dest, const uint8_t *tlvs, uint8_t count,
                             otReceiveDiagnosticGetCallback cb, void *ctx, bool filter_peer)
{
    mock_netdiag_sends++;
    mock_netdiag_filtered_sends += filter_peer;
    return otThreadSendDiagnosticGet(esp_openthread_get_instance(), dest, tlvs, count, cb, ctx);
}
unsigned mock_netdiag_releases;
void netdiag_mux_release(otReceiveDiagnosticGetCallback cb)
{
    (void)cb;
    mock_netdiag_releases++;
}
bool esp_openthread_lock_acquire(uint32_t timeout)
{
    (void)timeout;
    mock_lock_acquires++;
    if (mock_on_lock) {
        void (*hook)(void) = mock_on_lock;
        mock_on_lock = NULL;
        hook();
    }
    return true;
}
void esp_openthread_lock_release(void)
{
}
void esp_br_web_epskc_set_active_tap(const char *tap)
{
    (void)tap;
}
void esp_restart(void)
{
    assert(!"Unexpected restart");
}
void reboot_gate_restart(uint32_t delay_ms)
{
    (void)delay_ms;
    assert(!"Unexpected restart");
}
SemaphoreHandle_t xSemaphoreCreateBinary(void)
{
    return (SemaphoreHandle_t)&semaphore_storage;
}
extern void mock_complete_pending(void);
int xSemaphoreTake(SemaphoreHandle_t sem, uint32_t timeout)
{
    if (timeout) {
        mock_complete_pending();
    }
    bool ready = sem->ready;
    sem->ready = false;
    return ready;
}
int xSemaphoreGive(SemaphoreHandle_t sem)
{
    sem->ready = true;
    return 1;
}
bool mock_netcfg_deny;
netcfg_service_t mock_netcfg_last_service;
bool netcfg_request_allowed(httpd_req_t *req, netcfg_service_t svc)
{
    (void)req;
    mock_netcfg_last_service = svc;
    return !mock_netcfg_deny;
}
int xTaskCreatePinnedToCore(void (*task)(void *), const char *name, unsigned stack, void *arg, unsigned priority,
                            TaskHandle_t *handle, int core)
{
    (void)core;
    return xTaskCreate(task, name, stack, arg, priority, handle);
}
int xTaskCreate(void (*task)(void *), const char *name, unsigned stack, void *arg, unsigned priority,
                TaskHandle_t *handle)
{
    (void)task;
    (void)name;
    (void)stack;
    (void)arg;
    (void)priority;
    if (handle) {
        *handle = (void *)(uintptr_t)1;
    }
    return 1;
}
TickType_t mock_ticks;
void (*mock_on_delay)(void);
otNetworkDiagTlv mock_diag_tlvs[8];
unsigned mock_diag_tlv_count;
void vTaskDelay(uint32_t ticks)
{
    mock_ticks += ticks;
    if (mock_on_delay) {
        void (*hook)(void) = mock_on_delay;
        mock_on_delay = NULL;
        hook();
    }
}
TickType_t xTaskGetTickCount(void)
{
    return mock_ticks;
}
uint32_t esp_get_free_heap_size(void)
{
    return 1000000;
}
size_t strlcpy(char *dest, const char *src, size_t length)
{
    size_t size = strlen(src);
    if (length) {
        size_t n = size < length - 1 ? size : length - 1;
        memcpy(dest, src, n);
        dest[n] = 0;
    }
    return size;
}
static const char *header(httpd_req_t *req, const char *key)
{
    for (cJSON *item = req->headers ? req->headers->child : NULL; item; item = item->next) {
        if (!strcasecmp(item->string, key)) {
            return item->valuestring;
        }
    }
    return "";
}
size_t httpd_req_get_hdr_value_len(httpd_req_t *req, const char *key)
{
    return strlen(header(req, key));
}
int httpd_req_get_hdr_value_str(httpd_req_t *req, const char *key, char *value, size_t capacity)
{
    strlcpy(value, header(req, key), capacity);
    return 0;
}
int httpd_req_recv(httpd_req_t *req, char *buffer, size_t length)
{
    size_t available = strlen(req->input) - req->received;
    if (!available) {
        return -1;
    }
    if (length > available) {
        length = available;
    }
    if (req->fragment && length > req->fragment) {
        length = req->fragment;
    }
    memcpy(buffer, req->input + req->received, length);
    req->received += length;
    return length;
}
int httpd_resp_set_status(httpd_req_t *req, const char *status)
{
    req->borrowed_status = status;
    return 0;
}
int httpd_resp_set_type(httpd_req_t *req, const char *type)
{
    req->borrowed_type = type;
    return 0;
}
int httpd_resp_set_hdr(httpd_req_t *req, const char *key, const char *value)
{
    assert(req->header_count < 16);
    unsigned i = req->header_count++;
    req->borrowed_keys[i] = key;
    req->borrowed_values[i] = value;
    return 0;
}
int httpd_resp_send(httpd_req_t *req, const char *body, size_t length)
{
    req->status = strdup(req->borrowed_status);
    req->type = strdup(req->borrowed_type);
    req->body = calloc(length + 1, 1);
    if (length) {
        memcpy(req->body, body, length);
    }
    req->sent_headers = cJSON_CreateObject();
    for (unsigned i = 0; i < req->header_count; i++) {
        cJSON_AddStringToObject(req->sent_headers, req->borrowed_keys[i], req->borrowed_values[i]);
    }
    return 0;
}
int httpd_register_uri_handler(httpd_handle_t server, const httpd_uri_t *route)
{
    (void)server;
    assert(route->handler);
    mock_registered++;
    return 0;
}
int hex_to_string(const uint8_t *bytes, char *text, size_t size)
{
    for (size_t i = 0; i < size; i++) {
        snprintf(text + i * 2, 3, "%02x", bytes[i]);
    }
    text[size * 2] = 0;
    return 0;
}
