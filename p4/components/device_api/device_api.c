// /device/ endpoints (spec 5.7). Glue between esp_http_server and br_core / sniffer / esp_br_http_ota; the logic that
// can run on the host (JSON, query parsing, the one-long-operation rule, error mapping) is in the other files.
#include "device_api.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#include "board_cores.h"
#include "br_core.h"
#include "c6_board.h"
#include "esp_partition.h"
#include "ota_bundle.h"
#ifdef OTBR_TEST_BUILD
#include "board.h"
#include "driver/gpio.h"
#endif
#include "device_api_priv.h"
#include "diag_handler.h"
#include "metrics.h"
#include "esp_br_http_ota.h"
#include "netcfg.h"
#include "rcp_sync.h"
#include "sniffer.h"

static const char *TAG = "device_api";

#define WORKER_CORE MGMT_CORE // like the HTTP server task
#define WORKER_PRIORITY 5
#define OTA_STACK 8192
#define PUSH_STACK 10240
#define PCAP_STACK 6144
#define CHUNK_BYTES 4096
// The 2 MB web slot erase inside esp_br_ota_write stalls the upload for seconds: a recv waits at least this long.
#define LONG_RECV_TIMEOUT_S 60
#define SEND_TIMEOUT_S 10
#define DRAIN_RECV_TIMEOUT_S 2
#define DRAIN_MAX_BYTES (8 * 1024 * 1024)
#define PCAP_READ_MS 250 // sniffer_read timeout: the loop looks at disconnects and the end of the capture this often
#define PCAP_KEEPALIVE_US 1000000 // idle-capture liveness probe of the RCP, at most this often
#define PCAP_DEFAULT_DURATION_S 600
#define PCAP_MAX_DURATION_S 3600
#define PCAP_MIN_CHANNEL 11
#define PCAP_MAX_CHANNEL 26
#define REBOOT_DELAY_MS 1000
#define URL_MAX 256
#define URL_BODY_MAX 512
#define HTTP_CLIENT_TIMEOUT_MS 15000
#define OTA_URL_TIMEOUT_MS 30000
#define STATUS_BODY_MAX 1280
#define BASE_VERSION_MAX 32
#define ETH_IFKEY "ETH_DEF" // used until netcfg has created the interfaces
// url_source_open result (private): the response is larger than the C6 slot
#define DEVICE_API_ERR_TOO_LARGE 0x7a10

/* ---- replies ---- */

// JSON error reply. With `close_conn` the connection is closed afterwards (an unread request body would otherwise be
// parsed as the next request).
static esp_err_t send_error(httpd_req_t *req, int http_status, const char *code, const char *detail, bool close_conn)
{
    char body[384];

    if (device_api_error_json(body, sizeof(body), code, detail) < 0) {
        device_api_error_json(body, sizeof(body), code, "");
    }
    httpd_resp_set_status(req, device_api_status_line(http_status));
    httpd_resp_set_type(req, "application/json");
    if (close_conn) {
        httpd_resp_set_hdr(req, "Connection", "close");
    }
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t send_mapped_error(httpd_req_t *req, device_api_error_t error, const char *detail, bool close_conn)
{
    return send_error(req, error.http_status, error.code, detail, close_conn);
}

static esp_err_t send_json(httpd_req_t *req, const char *body)
{
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

/* ---- request helpers ---- */

// Raw query string of the request in a heap buffer, NULL when there is none (or no memory).
static char *request_query(httpd_req_t *req)
{
    const size_t len = httpd_req_get_url_query_len(req);

    if (len == 0) {
        return NULL;
    }
    char *query = malloc(len + 1);

    if (query && httpd_req_get_url_query_str(req, query, len + 1) != ESP_OK) {
        free(query);
        query = NULL;
    }
    return query;
}

typedef struct {
    struct timeval recv;
    struct timeval send;
} sock_timeouts_t;

// Set the socket timeouts of the request's connection; `saved` (may be NULL) receives the previous ones.
static void set_socket_timeouts(httpd_req_t *req, int recv_s, int send_s, sock_timeouts_t *saved)
{
    const int fd = httpd_req_to_sockfd(req);
    const struct timeval recv_tv = {.tv_sec = recv_s};
    const struct timeval send_tv = {.tv_sec = send_s};

    if (saved) {
        socklen_t len = sizeof(saved->recv);
        getsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &saved->recv, &len);
        len = sizeof(saved->send);
        getsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &saved->send, &len);
    }
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &recv_tv, sizeof(recv_tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &send_tv, sizeof(send_tv));
}

// End of an async request: the connection goes back to the server with its original timeouts.
static void finish_request(httpd_req_t *req, const sock_timeouts_t *saved)
{
    const int fd = httpd_req_to_sockfd(req);

    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &saved->recv, sizeof(saved->recv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &saved->send, sizeof(saved->send));
    httpd_req_async_handler_complete(req);
}

// Read and drop what is left of the request body (bounded), so that a client that is still sending can read our error
// reply instead of seeing a reset.
static void drain_body(httpd_req_t *req)
{
    char *buf = malloc(CHUNK_BYTES);
    size_t drained = 0;

    if (!buf) {
        return;
    }
    set_socket_timeouts(req, DRAIN_RECV_TIMEOUT_S, SEND_TIMEOUT_S, NULL);
    for (;;) {
        const int got = httpd_req_recv(req, buf, CHUNK_BYTES);
        if (got <= 0) {
            break;
        }
        drained += (size_t)got;
        if (drained > DRAIN_MAX_BYTES) {
            break;
        }
    }
    free(buf);
}

/* ---- reboot ---- */

static void reboot_now(void *arg)
{
    esp_restart();
}

static esp_timer_handle_t s_reboot_timer; // created once by device_api_register

static void schedule_reboot(uint32_t delay_ms)
{
    if (!s_reboot_timer || esp_timer_start_once(s_reboot_timer, (uint64_t)delay_ms * 1000) != ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
        esp_restart();
    }
}

// A long operation (OTA, firmware push) is not interrupted by a reboot request.
static esp_err_t reboot_post_handler(httpd_req_t *req)
{
    if (device_api_op_long_running()) {
        return send_error(req, 409, "busy", "an update or firmware push is running or waits for its reboot", false);
    }
    esp_err_t err = send_json(req, "{\"status\":\"ok\",\"detail\":\"rebooting\"}");

    schedule_reboot(REBOOT_DELAY_MS);
    return err;
}

/* ---- GET /device/status ---- */

static void fill_eth(device_api_status_t *s, char *ipv4_text, size_t ipv4_cap)
{
    // The status shows the management interface (the only one in single mode).
    esp_netif_t *netif = netcfg_mgmt_netif();
    esp_netif_ip_info_t ip4;

    if (!netif) {
        netif = esp_netif_get_handle_from_ifkey(ETH_IFKEY);
    }
    s->ipv4 = "";
    if (!netif) {
        return;
    }
    if (esp_netif_get_ip_info(netif, &ip4) == ESP_OK && ip4.ip.addr != 0) {
        snprintf(ipv4_text, ipv4_cap, IPSTR, IP2STR(&ip4.ip));
        s->ipv4 = ipv4_text;
    }
    esp_ip6_addr_t addrs[CONFIG_LWIP_IPV6_NUM_ADDRESSES];
    const int count = esp_netif_get_all_ip6(netif, addrs);

    for (int i = 0; i < count && s->ipv6_count < DEVICE_API_MAX_IPV6; i++) {
        snprintf(s->ipv6[s->ipv6_count++], DEVICE_API_ADDR_LEN, IPV6STR, IPV62STR(addrs[i]));
    }
}

static const uint8_t *s_base_version_start;
static const uint8_t *s_base_version_end;

void device_api_set_base_expected(const uint8_t *start, const uint8_t *end)
{
    s_base_version_start = start;
    s_base_version_end = end;
}

// Version text of the embedded C6 base image, NUL-terminated, trailing whitespace removed.
static void base_expected_version(char *out, size_t cap)
{
    size_t len = s_base_version_start ? (size_t)(s_base_version_end - s_base_version_start) : 0;

    len = len < cap - 1 ? len : cap - 1;
    if (len > 0) {
        memcpy(out, s_base_version_start, len);
    }
    while (len > 0 && (out[len - 1] == '\n' || out[len - 1] == '\r' || out[len - 1] == ' ')) {
        len--;
    }
    out[len] = '\0';
}

// Works in every mode and never waits long: br_core_get_status uses a short lock timeout and skips the lock in a fault.
static esp_err_t status_get_handler(httpd_req_t *req)
{
    device_api_status_t *s = calloc(1, sizeof(*s));
    char *body = malloc(STATUS_BODY_MAX);
    br_core_status_t bs;
    sniffer_stats_t ss;
    char ipv4_text[16];
    char base_expected[BASE_VERSION_MAX];

    if (!s || !body) {
        free(s);
        free(body);
        return send_error(req, 500, "no_memory", "out of memory", false);
    }
    br_core_get_status(&bs);
    sniffer_get_stats(&ss);
    rcp_sync_probe_result_t probe;

    rcp_sync_get_last_probe(&probe);
    const esp_partition_t *running = esp_ota_get_running_partition();

    base_expected_version(base_expected, sizeof(base_expected));
    s->mode = bs.mode;
    s->app_version = esp_app_get_description()->version;
    s->app_slot = running ? (int)(running->subtype - ESP_PARTITION_SUBTYPE_APP_OTA_0) : -1;
    s->rcp_installed_known = bs.rcp_installed_known;
    s->rcp_installed = bs.rcp_installed;
    s->rcp_compatible = bs.rcp_compatible;
    s->jumper_present = rcp_sync_jumper_present();
    s->jumper_recovery_failed = bs.rom_recovery_failed;
    s->base_expected = base_expected;
    s->base_installed = probe.updater_reachable ? probe.updater_version : "";
    fill_eth(s, ipv4_text, sizeof(ipv4_text));
    s->pd_state = bs.mode == BR_MODE_BR_RUNNING && !bs.ot_state_valid ? DEVICE_API_PD_STATE_UNKNOWN : bs.pd_state;
    s->pd_prefix = bs.pd_prefix;
    s->sniffer_active = ss.active;
    s->sniffer_ended_by_fault = ss.ended_by_fault;
    s->sniffer_channel = ss.channel;
    s->sniffer_frames = ss.frames;
    s->sniffer_drops = ss.drops;
    s->sniffer_elapsed_s = ss.elapsed_s;
    s->sniffer_duration_s = ss.duration_s;
    s->fault_reason = bs.fault_reason;
    s->storage_ok = !bs.storage_degraded;

    esp_err_t err;

    if (device_api_status_json(body, STATUS_BODY_MAX, s) < 0) {
        err = send_error(req, 500, "internal", "status does not fit its buffer", false);
    } else {
        err = send_json(req, body);
    }
    free(s);
    free(body);
    return err;
}

/* ---- GET /device/sniffer.pcap, DELETE /device/sniffer ---- */

typedef struct {
    httpd_req_t *req; // async copy
    uint8_t channel;
    uint32_t duration_s;
} pcap_job_t;

static device_api_error_t map_sniffer_start_error(esp_err_t err)
{
    switch (err) {
    case ESP_ERR_INVALID_STATE:
        return (device_api_error_t) {
            409, "busy", NULL
        };
    case ESP_ERR_INVALID_ARG:
        return (device_api_error_t) {
            400, "invalid_request", NULL
        };
    case ESP_ERR_NO_MEM:
        return (device_api_error_t) {
            500, "no_memory", NULL
        };
    default:
        return (device_api_error_t) {
            500, "sniffer_failed", NULL
        };
    }
}

// True when the peer closed the connection or it broke. A GET has no body, so nothing is expected to arrive.
static bool peer_gone(int fd)
{
    char byte;
    const int got = recv(fd, &byte, 1, MSG_PEEK | MSG_DONTWAIT);

    if (got == 0) {
        return true;
    }
    return got < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR;
}

// Streams the capture until it ends (duration, DELETE, fault), the client leaves, or a send fails. A failed send and a
// disconnect stop the capture (back to BR_RUNNING); a fault ends the response cleanly with the terminating chunk.
static void pcap_task(void *arg)
{
    pcap_job_t *job = arg;
    httpd_req_t *req = job->req;
    uint8_t *buf = malloc(CHUNK_BYTES);
    const int fd = httpd_req_to_sockfd(req);
    bool send_ok = true;
    bool started = false;
    int64_t last_keepalive_us = esp_timer_get_time();
    sock_timeouts_t saved;

    set_socket_timeouts(req, LONG_RECV_TIMEOUT_S, SEND_TIMEOUT_S, &saved);
    if (!buf) {
        send_error(req, 500, "no_memory", "out of memory", false);
        goto done;
    }
    const esp_err_t start_err = sniffer_start(job->channel, job->duration_s);

    if (start_err != ESP_OK) {
        send_mapped_error(req, map_sniffer_start_error(start_err), esp_err_to_name(start_err), false);
        goto done;
    }
    started = true;
    httpd_resp_set_type(req, "application/vnd.tcpdump.pcap");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"sniffer.pcap\"");
    for (;;) {
        const size_t n = sniffer_read(buf, CHUNK_BYTES, PCAP_READ_MS);

        if (n > 0) {
            if (httpd_resp_send_chunk(req, (const char *)buf, (ssize_t)n) != ESP_OK) {
                ESP_LOGW(TAG, "pcap send failed, stopping the capture");
                send_ok = false;
                break;
            }
            continue;
        }
        sniffer_stats_t stats;
        sniffer_get_stats(&stats);
        if (!stats.active) {
            if (stats.ended_by_fault) {
                ESP_LOGE(TAG, "capture ended by a radio fault after %u frames", (unsigned)stats.frames);
            }
            break;
        }
        if (esp_timer_get_time() - last_keepalive_us >= PCAP_KEEPALIVE_US) {
            last_keepalive_us = esp_timer_get_time();
            br_core_sniffer_keepalive(); // an idle capture would not notice a dead RCP otherwise
        }
        if (peer_gone(fd)) {
            ESP_LOGI(TAG, "pcap client disconnected, stopping the capture");
            send_ok = false;
            break;
        }
    }
    if (started) {
        sniffer_stop(); // no-op when the capture ended on its own
    }
    if (send_ok) {
        httpd_resp_send_chunk(req, NULL, 0);
    }
done:
    free(buf);
    free(job);
    finish_request(req, &saved);
    device_api_op_end(DEVICE_API_OP_PCAP);
    vTaskDelete(NULL);
}

static esp_err_t pcap_get_handler(httpd_req_t *req)
{
    char *query = request_query(req);
    uint32_t channel = 0;
    uint32_t duration = PCAP_DEFAULT_DURATION_S;
    const esp_err_t channel_err = device_api_query_uint(query, "channel", PCAP_MIN_CHANNEL, PCAP_MAX_CHANNEL, &channel);
    const esp_err_t duration_err = device_api_query_uint(query, "duration", 1, PCAP_MAX_DURATION_S, &duration);

    free(query);
    if (channel_err != ESP_OK && channel_err != ESP_ERR_NOT_FOUND) {
        return send_error(req, 400, "invalid_channel", "channel must be 11..26", false);
    }
    if (duration_err != ESP_OK && duration_err != ESP_ERR_NOT_FOUND) {
        return send_error(req, 400, "invalid_duration", "duration must be 1..3600 seconds", false);
    }
    br_core_status_t status;

    br_core_get_status(&status);
    if (status.mode != BR_MODE_BR_RUNNING) {
        return send_error(req, 409, "busy",
                          status.mode == BR_MODE_SNIFFER ? "a capture is already running" : "the border router is not running", false);
    }
    if (channel_err == ESP_ERR_NOT_FOUND) {
        const device_api_error_t error = device_api_pcap_default_channel_error(status.ot_state_valid, status.has_dataset);

        if (error.http_status != 0) {
            return send_mapped_error(req, error, error.detail, false);
        }
        channel = status.channel;
    }
    if (!device_api_op_try_begin(DEVICE_API_OP_PCAP)) {
        return send_error(req, 409, "busy", device_api_op_busy_detail(DEVICE_API_OP_PCAP), false);
    }
    pcap_job_t *job = calloc(1, sizeof(*job));
    httpd_req_t *async_req = NULL;

    if (!job || httpd_req_async_handler_begin(req, &async_req) != ESP_OK) {
        free(job);
        device_api_op_end(DEVICE_API_OP_PCAP);
        return send_error(req, 500, "internal", "cannot start the capture stream", false);
    }
    job->req = async_req;
    job->channel = (uint8_t)channel;
    job->duration_s = duration;
    if (xTaskCreatePinnedToCore(pcap_task, "pcap_stream", PCAP_STACK, job, WORKER_PRIORITY, NULL, WORKER_CORE) != pdPASS) {
        send_error(async_req, 500, "no_memory", "cannot start the capture task", false);
        httpd_req_async_handler_complete(async_req);
        free(job);
        device_api_op_end(DEVICE_API_OP_PCAP);
    }
    return ESP_OK;
}

static esp_err_t sniffer_delete_handler(httpd_req_t *req)
{
    sniffer_stats_t stats;

    sniffer_get_stats(&stats);
    if (!stats.active) {
        return send_error(req, 404, "not_found", "no capture is running", false);
    }
    const esp_err_t err = sniffer_stop(); // the stream task sees the end within its read timeout and finishes the response

    if (err != ESP_OK) {
        return send_error(req, 500, "sniffer_failed", esp_err_to_name(err), false);
    }
    return send_json(req, "{\"status\":\"ok\",\"detail\":\"capture stopped\"}");
}

/* ---- POST /device/ota, POST /device/ota/url ---- */

typedef struct {
    httpd_req_t *req; // async copy
    char url[URL_MAX + 1]; // empty = upload
} ota_job_t;

// Reply 200 first, reboot ~1 s later. The operation slot stays taken: the update waits for its reboot.
static void ota_succeeded(httpd_req_t *req, const sock_timeouts_t *saved)
{
    send_json(req, "{\"status\":\"ok\",\"detail\":\"update installed, rebooting\"}");
    finish_request(req, saved);
    schedule_reboot(REBOOT_DELAY_MS);
}

static void ota_task(void *arg)
{
    ota_job_t *job = arg;
    httpd_req_t *req = job->req;
    const bool upload = job->url[0] == '\0';
    esp_err_t err;
    sock_timeouts_t saved;

    set_socket_timeouts(req, LONG_RECV_TIMEOUT_S, SEND_TIMEOUT_S, &saved);
    if (upload) {
        err = esp_br_ota_from_httpd(req);
    } else {
        esp_http_client_config_t config = {.url = job->url, .timeout_ms = OTA_URL_TIMEOUT_MS};
        err = esp_br_http_ota(&config);
    }
    if (err == ESP_OK) {
        ota_succeeded(req, &saved);
    } else {
        ESP_LOGW(TAG, "update failed: %s", esp_err_to_name(err));
        if (upload) {
            drain_body(req);
        }
        const device_api_error_t error = device_api_map_ota_error(err);

        send_mapped_error(req, error, error.detail ? error.detail : esp_err_to_name(err), upload);
        finish_request(req, &saved);
        device_api_op_end(DEVICE_API_OP_OTA);
    }
    free(job);
    vTaskDelete(NULL);
}

// Take the OTA slot, detach the request and start the worker. Replies (and releases) on any failure.
static esp_err_t start_ota_job(httpd_req_t *req, const char *url)
{
    const bool upload = url == NULL;

    if (!device_api_op_try_begin(DEVICE_API_OP_OTA)) {
        return send_error(req, 409, "busy", device_api_op_busy_detail(DEVICE_API_OP_OTA), upload);
    }
    ota_job_t *job = calloc(1, sizeof(*job));
    httpd_req_t *async_req = NULL;

    if (!job || httpd_req_async_handler_begin(req, &async_req) != ESP_OK) {
        free(job);
        device_api_op_end(DEVICE_API_OP_OTA);
        return send_error(req, 500, "internal", "cannot start the update", upload);
    }
    job->req = async_req;
    if (url) {
        strlcpy(job->url, url, sizeof(job->url));
    }
    if (xTaskCreatePinnedToCore(ota_task, "ota_http", OTA_STACK, job, WORKER_PRIORITY, NULL, WORKER_CORE) != pdPASS) {
        send_error(async_req, 500, "no_memory", "cannot start the update task", upload);
        httpd_req_async_handler_complete(async_req);
        free(job);
        device_api_op_end(DEVICE_API_OP_OTA);
    }
    return ESP_OK;
}

// Largest possible bundle: header, web image (at most the web partition), app image (at most the app partition).
static size_t ota_max_bundle_bytes(void)
{
    const esp_partition_t *web = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, NULL);
    const esp_partition_t *app = esp_ota_get_next_update_partition(NULL);

    return OTA_BUNDLE_HEADER_LEN + (web ? web->size : 0) + (app ? app->size : 0);
}

// Length rule shared by the upload handlers: the reply for a missing or absurd Content-Length, before anything is touched.
static esp_err_t send_length_error(httpd_req_t *req, int http_status, const char *what)
{
    return http_status == 413 ? send_error(req, 413, "payload_too_large", what, true)
           : send_error(req, 400, "length_required", what, true);
}

static esp_err_t ota_post_handler(httpd_req_t *req)
{
    const int length_status = device_api_length_check(req->content_len, ota_max_bundle_bytes());

    if (length_status != 0) {
        return send_length_error(req, length_status, length_status == 413 ? "the bundle is larger than both partitions"
                                 : "the bundle needs a Content-Length");
    }
    return start_ota_job(req, NULL);
}

// {"url": "..."} -> url in `out`; false when the body is not that.
static bool parse_url_body(const char *body, char *out, size_t cap)
{
    cJSON *json = cJSON_Parse(body);
    const cJSON *item = json ? cJSON_GetObjectItemCaseSensitive(json, "url") : NULL;
    const bool ok = cJSON_IsString(item) && item->valuestring && strlen(item->valuestring) < cap;

    if (ok) {
        strlcpy(out, item->valuestring, cap);
    }
    cJSON_Delete(json);
    return ok;
}

static esp_err_t ota_url_post_handler(httpd_req_t *req)
{
    if (req->content_len == 0 || req->content_len > URL_BODY_MAX) {
        return send_error(req, 400, "invalid_request", "body must be {\"url\": \"http://...\"}", true);
    }
    char body[URL_BODY_MAX + 1];
    size_t got = 0;

    while (got < req->content_len) {
        const int n = httpd_req_recv(req, body + got, req->content_len - got);
        if (n <= 0) {
            return send_error(req, 400, "invalid_request", "request body not received", true);
        }
        got += (size_t)n;
    }
    body[got] = '\0';
    char url[URL_MAX + 1];

    if (!parse_url_body(body, url, sizeof(url))) {
        return send_error(req, 400, "invalid_request", "body must be {\"url\": \"http://...\"}", false);
    }
    const device_api_error_t url_error = device_api_check_url(url);

    if (url_error.http_status != 0) {
        return send_mapped_error(req, url_error, url_error.detail, false);
    }
    return start_ota_job(req, url);
}

/* ---- POST /device/radio/firmware ---- */

#ifdef OTBR_TEST_BUILD
static uint32_t s_inject_reset_at; // 0 = off

void device_api_test_inject_c6_reset_at(uint32_t offset)
{
    s_inject_reset_at = offset;
}

// Test builds: reset the C6 once the source is read up to the configured offset (a reset in the middle of a transfer).
static void maybe_inject_reset(size_t offset, size_t len)
{
    const uint32_t at = s_inject_reset_at;

    if (at != 0 && offset <= at && at < offset + len) {
        s_inject_reset_at = 0;
        ESP_LOGW(TAG, "test build: resetting the C6 at offset %u", (unsigned)offset);
        gpio_set_level(P4_PIN_C6_RESET, 0);
        vTaskDelay(pdMS_TO_TICKS(20));
        gpio_set_level(P4_PIN_C6_RESET, 1);
    }
}
#else
static void maybe_inject_reset(size_t offset, size_t len)
{
}
#endif

typedef struct {
    httpd_req_t *req; // async copy
    char url[URL_MAX + 1]; // empty = body upload
} push_job_t;

// One-shot source: the request body.
typedef struct {
    httpd_req_t *req;
    size_t got;
} body_source_t;

static esp_err_t body_source_read(void *ctx, size_t offset, void *buf, size_t len)
{
    body_source_t *src = ctx;

    if (offset != src->got) {
        return ESP_ERR_INVALID_STATE; // the source is sequential and not re-openable
    }
    size_t n = 0;

    maybe_inject_reset(offset, len);
    while (n < len) {
        const int r = httpd_req_recv(src->req, (char *)buf + n, len - n);
        if (r <= 0) {
            ESP_LOGE(TAG, "firmware upload interrupted (%d) at %u", r, (unsigned)(src->got + n));
            return ESP_FAIL;
        }
        n += (size_t)r;
    }
    src->got += n;
    return ESP_OK;
}

// Re-openable source: an HTTP GET through esp_http_client.
typedef struct {
    esp_http_client_handle_t client;
    int64_t length;
    size_t got;
} url_source_t;

// Open the connection and read the response headers. ESP_ERR_NOT_FOUND = reachable but not a usable 200.
static esp_err_t url_source_open(url_source_t *src)
{
    ESP_RETURN_ON_ERROR(esp_http_client_open(src->client, 0), TAG, "connecting failed");
    const int64_t length = esp_http_client_fetch_headers(src->client);
    const int status = esp_http_client_get_status_code(src->client);

    if (status != 200) {
        esp_http_client_close(src->client);
        return ESP_ERR_NOT_FOUND;
    }
    if (length > C6_OTA_SLOT_SIZE) {
        esp_http_client_close(src->client);
        return DEVICE_API_ERR_TOO_LARGE; // larger than the C6 slot: refused before anything is touched
    }
    if (length <= 0 || (src->length != 0 && length != src->length)) {
        esp_http_client_close(src->client);
        return ESP_ERR_INVALID_SIZE;
    }
    src->length = length;
    src->got = 0;
    return ESP_OK;
}

static esp_err_t url_source_reopen(void *ctx)
{
    url_source_t *src = ctx;

    esp_http_client_close(src->client);
    return url_source_open(src);
}

static esp_err_t url_source_read(void *ctx, size_t offset, void *buf, size_t len)
{
    url_source_t *src = ctx;

    if (offset != src->got) {
        return ESP_ERR_INVALID_STATE;
    }
    size_t n = 0;

    maybe_inject_reset(offset, len);
    while (n < len) {
        const int r = esp_http_client_read(src->client, (char *)buf + n, (int)(len - n));
        if (r <= 0) {
            ESP_LOGE(TAG, "firmware download interrupted (%d) at %u", r, (unsigned)(src->got + n));
            return ESP_FAIL;
        }
        n += (size_t)r;
    }
    src->got += n;
    return ESP_OK;
}

static void push_finish(httpd_req_t *req, const sock_timeouts_t *saved)
{
    finish_request(req, saved);
    device_api_op_end(DEVICE_API_OP_RADIO_PUSH);
}

static void push_reply_result(httpd_req_t *req, esp_err_t err, const char *detail, bool upload)
{
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "radio firmware push failed: %s (%s)", esp_err_to_name(err), detail);
        if (upload) {
            drain_body(req);
        }
        send_mapped_error(req, device_api_map_push_error(err), detail[0] ? detail : esp_err_to_name(err), upload);
        return;
    }
    // After an abandoned OpenThread start the mode stays RADIO_FAULT for ~1.5 s until br_core reboots by itself:
    // the reply goes out at once.
    br_core_status_t status;
    char body[96];

    br_core_get_status(&status);
    if (device_api_installed_json(body, sizeof(body), status.rcp_installed_known, status.rcp_installed) < 0) {
        send_error(req, 500, "internal", "reply does not fit its buffer", false);
        return;
    }
    send_json(req, body);
}

static void push_task(void *arg)
{
    push_job_t *job = arg;
    httpd_req_t *req = job->req;
    const bool upload = job->url[0] == '\0';
    char detail[96] = "";
    body_source_t body_src = {.req = req};
    url_source_t url_src = {0};
    rcp_fw_source_t source = {0};
    esp_err_t err = ESP_OK;
    sock_timeouts_t saved;

    set_socket_timeouts(req, LONG_RECV_TIMEOUT_S, SEND_TIMEOUT_S, &saved);
    if (upload) {
        source = (rcp_fw_source_t) {
            .total_size = req->content_len, .read = body_source_read, .ctx = &body_src
        };
    } else {
        const esp_http_client_config_t config = {.url = job->url, .timeout_ms = HTTP_CLIENT_TIMEOUT_MS};

        url_src.client = esp_http_client_init(&config);
        err = url_src.client ? url_source_open(&url_src) : ESP_ERR_NO_MEM;
        if (err != ESP_OK) {
            // Nothing was touched: the border router keeps running.
            const char *why = err == ESP_ERR_NOT_FOUND ? "the URL did not answer 200"
                              : err == ESP_ERR_INVALID_SIZE ? "the URL gives no Content-Length" : "the URL is not reachable";
            if (err == DEVICE_API_ERR_TOO_LARGE) {
                send_error(req, 413, "payload_too_large", "the image is larger than the C6 slot", false);
            } else if (err == ESP_ERR_INVALID_SIZE) {
                send_error(req, 400, "length_required", why, false);
            } else if (err == ESP_ERR_NO_MEM) {
                send_error(req, 500, "no_memory", "out of memory", false);
            } else {
                send_error(req, 502, "fetch_failed", why, false);
            }
            goto done;
        }
        source = (rcp_fw_source_t) {
            .total_size = (size_t)url_src.length, .read = url_source_read, .reopen = url_source_reopen, .ctx = &url_src,
        };
    }
    err = br_core_push_radio_firmware(&source, detail, sizeof(detail));
    push_reply_result(req, err, detail, upload);
done:
    if (url_src.client) {
        esp_http_client_cleanup(url_src.client);
    }
    push_finish(req, &saved);
    free(job);
    vTaskDelete(NULL);
}

static esp_err_t radio_firmware_post_handler(httpd_req_t *req)
{
    char *query = request_query(req);
    char url[URL_MAX + 1] = "";
    const esp_err_t url_err = device_api_query_value(query, "from_url", url, sizeof(url));

    free(query);
    const bool from_url = url_err == ESP_OK;
    const bool upload = !from_url;

    if (url_err != ESP_OK && url_err != ESP_ERR_NOT_FOUND) {
        return send_error(req, 400, "invalid_request", "from_url is malformed or too long", true);
    }
    if (from_url) {
        const device_api_error_t url_error = device_api_check_url(url);

        if (url_error.http_status != 0) {
            return send_mapped_error(req, url_error, url_error.detail, true);
        }
    }
    if (from_url && req->content_len != 0) {
        return send_error(req, 400, "invalid_request", "from_url takes an empty body", true);
    }
    if (upload) {
        // Before the long-operation slot and before anything is torn down: the C6 slot size is the hard upper bound.
        const int length_status = device_api_length_check(req->content_len, C6_OTA_SLOT_SIZE);

        if (length_status != 0) {
            return send_length_error(req, length_status, length_status == 413 ? "the image is larger than the C6 slot"
                                     : "the image needs a Content-Length");
        }
    }
    const br_core_mode_t mode = br_core_get_mode();

    if (mode == BR_MODE_SNIFFER || mode == BR_MODE_STARTING) {
        return send_error(req, 409, "busy", mode == BR_MODE_SNIFFER ? "a capture is running" : "the border router is starting", upload);
    }
    if (!device_api_op_try_begin(DEVICE_API_OP_RADIO_PUSH)) {
        return send_error(req, 409, "busy", device_api_op_busy_detail(DEVICE_API_OP_RADIO_PUSH), upload);
    }
    push_job_t *job = calloc(1, sizeof(*job));
    httpd_req_t *async_req = NULL;

    if (!job || httpd_req_async_handler_begin(req, &async_req) != ESP_OK) {
        free(job);
        device_api_op_end(DEVICE_API_OP_RADIO_PUSH);
        return send_error(req, 500, "internal", "cannot start the firmware push", upload);
    }
    job->req = async_req;
    strlcpy(job->url, url, sizeof(job->url));
    if (xTaskCreatePinnedToCore(push_task, "radio_fw", PUSH_STACK, job, WORKER_PRIORITY, NULL, WORKER_CORE) != pdPASS) {
        send_error(async_req, 500, "no_memory", "cannot start the push task", upload);
        httpd_req_async_handler_complete(async_req);
        free(job);
        device_api_op_end(DEVICE_API_OP_RADIO_PUSH);
    }
    return ESP_OK;
}

/* ---- POST /device/radio/recover, GET /device/radio/log (maintenance jumper, spec 5.10) ---- */

#define RECOVER_STACK 8192

static void recover_task(void *arg)
{
    httpd_req_t *req = arg;
    char detail[96] = "";
    const esp_err_t err = br_core_rom_recover(detail, sizeof(detail));
    br_core_status_t bs;
    char body[384];
    int http_status = 0;

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ROM recovery failed: %s (%s)", esp_err_to_name(err), detail);
    }
    br_core_get_status(&bs); // the state after the restart: BR_RUNNING with the RCP, or RADIO_FAULT "no rcp firmware"
    if (device_api_recover_reply(err, detail[0] ? detail : esp_err_to_name(err), bs.mode, bs.fault_reason, bs.rcp_installed_known, bs.rcp_installed,
                                 body, sizeof(body), &http_status) < 0) {
        http_status = 500;
        strlcpy(body, "{\"error\":\"internal\",\"detail\":\"reply too large\"}", sizeof(body));
    }
    httpd_resp_set_status(req, device_api_status_line(http_status));
    send_json(req, body);
    httpd_req_async_handler_complete(req);
    device_api_op_end(DEVICE_API_OP_ROM_RECOVER);
    vTaskDelete(NULL);
}

static esp_err_t radio_recover_post_handler(httpd_req_t *req)
{
    if (req->content_len != 0) {
        return send_error(req, 400, "invalid_request", "the request takes no body", true);
    }
    if (!rcp_sync_jumper_present()) {
        const device_api_error_t error = device_api_map_recover_error(ESP_ERR_NOT_SUPPORTED);

        return send_mapped_error(req, error, error.detail, false);
    }
    const br_core_mode_t mode = br_core_get_mode();

    if (mode == BR_MODE_SNIFFER || mode == BR_MODE_STARTING) {
        return send_error(req, 409, "busy", mode == BR_MODE_SNIFFER ? "a capture is running" : "the border router is starting", false);
    }
    if (!device_api_op_try_begin(DEVICE_API_OP_ROM_RECOVER)) {
        return send_error(req, 409, "busy", device_api_op_busy_detail(DEVICE_API_OP_ROM_RECOVER), false);
    }
    httpd_req_t *async_req = NULL;

    if (httpd_req_async_handler_begin(req, &async_req) != ESP_OK) {
        device_api_op_end(DEVICE_API_OP_ROM_RECOVER);
        return send_error(req, 500, "internal", "cannot start the recovery", false);
    }
    if (xTaskCreatePinnedToCore(recover_task, "rom_recover", RECOVER_STACK, async_req, WORKER_PRIORITY, NULL, WORKER_CORE) != pdPASS) {
        send_error(async_req, 500, "no_memory", "cannot start the recovery task", false);
        httpd_req_async_handler_complete(async_req);
        device_api_op_end(DEVICE_API_OP_ROM_RECOVER);
    }
    return ESP_OK;
}

static esp_err_t radio_log_get_handler(httpd_req_t *req)
{
    if (!rcp_sync_jumper_present()) {
        const device_api_error_t error = device_api_map_recover_error(ESP_ERR_NOT_SUPPORTED);

        return send_mapped_error(req, error, error.detail, false);
    }
    char *text = heap_caps_malloc(RCP_SYNC_LOG_CAPACITY, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (!text) {
        text = malloc(RCP_SYNC_LOG_CAPACITY);
    }
    if (!text) {
        return send_error(req, 500, "no_memory", "out of memory", false);
    }
    const size_t n = rcp_sync_log_read(text, RCP_SYNC_LOG_CAPACITY);

    httpd_resp_set_type(req, "text/plain");
    const esp_err_t err = httpd_resp_send(req, text, (ssize_t)n);

    free(text);
    return err;
}

/* ---- /device/network (spec 5.13) ---- */

#define NETWORK_BODY_MAX NETCFG_JSON_MAX

// Reads the whole body (at most NETWORK_BODY_MAX bytes, checked by the caller) into a NUL-terminated heap buffer.
static char *read_small_body(httpd_req_t *req)
{
    char *body = malloc(req->content_len + 1);

    for (size_t got = 0; body && got < req->content_len;) {
        const int n = httpd_req_recv(req, body + got, req->content_len - got);

        if (n <= 0) {
            free(body);
            return NULL;
        }
        got += (size_t)n;
    }
    if (body) {
        body[req->content_len] = '\0';
    }
    return body;
}

static esp_err_t network_get_handler(httpd_req_t *req)
{
    char *json = netcfg_state_json();

    if (!json) {
        return send_error(req, 503, "not_started", "the network configuration is not running", false);
    }
    const esp_err_t err = send_json(req, json);

    free(json);
    return err;
}

static esp_err_t network_put_handler(httpd_req_t *req)
{
    const int length_status = device_api_length_check(req->content_len, NETWORK_BODY_MAX);

    if (length_status != 0) {
        return send_error(req, length_status, length_status == 413 ? "payload_too_large" : "length_required",
                          "send the network configuration as a JSON body", true);
    }
    // A reboot into the new config would cut an update or a firmware push short.
    if (device_api_op_long_running()) {
        return send_error(req, 409, "busy", "an update or firmware push is running or waits for its reboot", true);
    }
    char *body = read_small_body(req);

    if (!body) {
        return send_error(req, 400, "invalid_request", "the body could not be read", true);
    }
    netcfg_config_t cfg;
    const char *bad_field = NULL;
    esp_err_t err = netcfg_from_json(body, &cfg, &bad_field);

    free(body);
    if (err == ESP_OK) {
        err = netcfg_set_trial(&cfg);
    }
    if (err != ESP_OK) {
        const device_api_error_t error = device_api_map_network_set_error(err);

        return send_mapped_error(req, error, err == ESP_ERR_INVALID_ARG && bad_field ? bad_field : error.detail, false);
    }
    return send_json(req, "{\"status\":\"trial\",\"detail\":\"rebooting into the new network configuration; confirm within 300 s on the management interface\"}");
}

static esp_err_t network_confirm_post_handler(httpd_req_t *req)
{
    if (req->content_len != 0) {
        return send_error(req, 400, "invalid_request", "the request takes no body", true);
    }
    const esp_err_t err = netcfg_confirm(req);

    if (err != ESP_OK) {
        const device_api_error_t error = device_api_map_network_confirm_error(err);

        return send_mapped_error(req, error, error.detail, false);
    }
    return send_json(req, "{\"status\":\"confirmed\"}");
}

/* ---- registration ---- */

typedef struct {
    const char *uri;
    httpd_method_t method;
    esp_err_t (*handler)(httpd_req_t *req);
} route_t;

// Every /device/ route goes through the per-interface policy first (spec 5.13); the route is in user_ctx.
static esp_err_t gated_handler(httpd_req_t *req)
{
    const route_t *route = req->user_ctx;

    if (!netcfg_request_allowed(req, NETCFG_SVC_DEVICE_API)) {
        return ESP_FAIL; // 403 is sent; failing the handler makes the server close the connection
    }
    return route->handler(req);
}

esp_err_t device_api_register(httpd_handle_t server)
{
    ESP_RETURN_ON_FALSE(server != NULL, ESP_ERR_INVALID_ARG, TAG, "no server");
    if (!s_reboot_timer) { // once, before any handler can run
        const esp_timer_create_args_t args = {.callback = reboot_now, .name = "device_reboot"};
        ESP_RETURN_ON_ERROR(esp_timer_create(&args, &s_reboot_timer), TAG, "creating the reboot timer failed");
    }
    static const route_t routes[] = {
        {"/device/status", HTTP_GET, status_get_handler},
        {"/device/sniffer.pcap", HTTP_GET, pcap_get_handler},
        {"/device/sniffer", HTTP_DELETE, sniffer_delete_handler},
        {"/device/ota", HTTP_POST, ota_post_handler},
        {"/device/ota/url", HTTP_POST, ota_url_post_handler},
        {"/device/radio/firmware", HTTP_POST, radio_firmware_post_handler},
        {"/device/radio/recover", HTTP_POST, radio_recover_post_handler},
        {"/device/radio/log", HTTP_GET, radio_log_get_handler},
        {"/device/reboot", HTTP_POST, reboot_post_handler},
        {"/device/diag", HTTP_GET, device_api_diag_get_handler},
        {"/device/network", HTTP_GET, network_get_handler},
        {"/device/network", HTTP_PUT, network_put_handler},
        {"/device/network/confirm", HTTP_POST, network_confirm_post_handler},
        {"/metrics", HTTP_GET, metrics_get_handler},
    };

    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        const httpd_uri_t uri = {.uri = routes[i].uri, .method = routes[i].method, .handler = gated_handler, .user_ctx = (void *) &routes[i]};

        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &uri), TAG, "registering %s failed", routes[i].uri);
    }
    return ESP_OK;
}
