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
#include "builtin_rcp.h"
#include "c6_board.h"
#include "esp_partition.h"
#include "ota_bundle.h"
#ifdef OTBR_TEST_BUILD
#include "board.h"
#include "driver/gpio.h"
#endif
#include "channels_json.h"
#include "device_api_priv.h"
#include "diag_handler.h"
#include "mesh_scan.h"
#include "metrics.h"
#include "esp_br_http_ota.h"
#include "self_update.h"
#include "self_update_http.h"
#include "self_update_priv.h"
#include "update_json.h"
#include "netcfg.h"
#include "names_json.h"
#include "console_json.h"
#include "console_ws.h"
#include "ot_console.h"
#include "thread_json.h"
#include "zigbee.h"
#include "zigbee_json.h"
#include "netnames.h"
#include "radio_mux.h"
#include "rcp_proxy.h"
#include "rcp_sync.h"
#include "reboot_gate.h"
#include "routing.h"
#include "sniffer.h"

static const char *TAG = "device_api";

#define WORKER_CORE MGMT_CORE // like the HTTP server task
#define WORKER_PRIORITY 5
#define OTA_STACK 8192
#define PUSH_STACK 10240
// The versions.txt fetch keeps ~1.5 KB on the stack, on top of a TLS handshake.
#define CHECK_STACK 8192
// The install runs the versions.txt fetch, the bundle download and the radio push one after the other on this stack.
#define UPDATE_STACK 12288
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
#define STATUS_BODY_MAX 3072
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

bool device_api_long_op_running(void)
{
    return device_api_op_long_running();
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

// The management interface (the only one in single mode), or the Ethernet netif until netcfg has created the interfaces.
static esp_netif_t *mgmt_netif(void)
{
    esp_netif_t *netif = netcfg_mgmt_netif();

    return netif ? netif : esp_netif_get_handle_from_ifkey(ETH_IFKEY);
}

static int running_slot(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();

    return running ? (int)(running->subtype - ESP_PARTITION_SUBTYPE_APP_OTA_0) : -1;
}

static void fill_eth(device_api_status_t *s, char *ipv4_text, size_t ipv4_cap)
{
    // The status shows the management interface.
    esp_netif_t *netif = mgmt_netif();
    esp_netif_ip_info_t ip4;

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

// The routing figures of /device/status from the task's last published status (no OpenThread lock). Routing not
// running: routing_started stays false and "routing" is null.
static void fill_routing(device_api_status_t *s)
{
    routing_settings_t settings;
    routing_status_t status;

    if (routing_get(&settings, &status) != ESP_OK) {
        return;
    }
    s->routing_started = true;
    s->routing_method = settings.method;
    s->routing_omr_mode = settings.omr_mode;
    s->routing_announced_count = status.announced.count;
    s->routing_peers_configured = settings.peer_count;
    for (size_t i = 0; i < settings.peer_count && i < ROUTING_MAX_PEERS; i++) {
        /* by address: right after a PUT the status can still be the one of the previous peers */
        const routing_bgp_peer_status_t *p = routing_status_peer(&status, settings.peers[i].addr);
        if (p && p->state == ROUTING_BGP_ESTABLISHED) {
            s->routing_peers_established++;
        }
    }
}

// Works in every mode and never waits long: br_core_get_status uses a short lock timeout and skips the lock in a fault.
static void fill_trel(device_api_trel_t *out)
{
    br_core_trel_t *t = calloc(1, sizeof(*t));

    memset(out, 0, sizeof(*out));
    if (!t) {
#if CONFIG_OPENTHREAD_RADIO_TREL
        out->built = true; // state_valid false: the state is written as unknown
#endif
        out->enabled = netcfg_trel_enabled();
        return;
    }
    br_core_get_trel(t);
    out->built = t->built;
    out->enabled = t->enabled;
    out->state_valid = t->state.state_valid;
    out->active = t->state.active;
    out->peers = t->state.peers;
    out->peers_other = t->state.peers_other;
    out->tx_packets = t->state.tx_packets;
    out->tx_failures = t->state.tx_failures;
    out->rx_packets = t->state.rx_packets;
    out->peer_count = t->list.count < DEVICE_API_TREL_PEERS_MAX ? t->list.count : DEVICE_API_TREL_PEERS_MAX;
    for (size_t i = 0; i < out->peer_count; i++) {
        strlcpy(out->peer_list[i].ext_address, t->list.peers[i].ext_address, sizeof(out->peer_list[i].ext_address));
        strlcpy(out->peer_list[i].ext_pan_id, t->list.peers[i].ext_pan_id, sizeof(out->peer_list[i].ext_pan_id));
        out->peer_list[i].same_network = t->list.peers[i].same_network;
        out->peer_list[i].neighbor = t->list.peers[i].neighbor;
        out->peer_list[i].pref_154 = t->list.peers[i].pref_154;
        out->peer_list[i].pref_trel = t->list.peers[i].pref_trel;
        strlcpy(out->peer_list[i].address, t->list.peers[i].sock_address, sizeof(out->peer_list[i].address));
    }
    free(t);
}

static esp_err_t status_get_handler(httpd_req_t *req)
{
    device_api_status_t *s = calloc(1, sizeof(*s));
    char *body = malloc(STATUS_BODY_MAX);
    br_core_status_t bs;
    builtin_rcp_status_t rs;
    sniffer_stats_t ss;
    char ipv4_text[16];
    char base_expected[BASE_VERSION_MAX];

    if (!s || !body) {
        free(s);
        free(body);
        return send_error(req, 500, "no_memory", "out of memory", false);
    }
    br_core_get_status(&bs);
    builtin_rcp_get_status(&rs);
    sniffer_get_stats(&ss);
    rcp_sync_probe_result_t probe;

    rcp_sync_get_last_probe(&probe);
    base_expected_version(base_expected, sizeof(base_expected));
    s->mode = bs.mode;
    s->app_version = esp_app_get_description()->version;
    s->app_slot = running_slot();
    s->rcp_installed_known = rs.installed_known;
    s->rcp_installed = rs.installed;
    s->rcp_compatible = bs.rcp_compatible;
    s->rcp_compatible_known = bs.rcp_compatible_known;
    s->jumper_present = rs.jumper;
    s->jumper_recovery_failed = rs.rom_recovery_failed;
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
    s->otbr_channel = bs.otbr_channel;
    fill_routing(s);
    fill_trel(&s->trel);
    s->zigbee_channel = zigbee_channel();

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

/* ---- GET /metrics ---- */

static esp_err_t send_metrics_chunk(void *ctx, const char *data, size_t len)
{
    return httpd_resp_send_chunk((httpd_req_t *)ctx, data, (ssize_t)len);
}

// The Zigbee series exist only while a channel runs ziggurat.
static void fill_zigbee_metrics(metrics_device_t *device)
{
    zigbee_status_t *st;

    if (!zigbee_channel() || !(st = malloc(sizeof(*st)))) {
        return;
    }
    zigbee_get_status(st);
    device->zigbee = (metrics_zigbee_t) {
        .active = true,
        .frames_rx = st->counters.rx,
        .frames_tx = st->counters.tx,
        .tx_no_ack = st->counters.tx_no_ack,
        .tx_cca = st->counters.tx_cca,
        .tx_failed = st->counters.tx_failed,
        .rcp_resets = st->counters.rcp_resets,
        .heap_current = st->heap.current,
        .heap_peak = st->heap.peak,
        .client_connected = st->has_client,
    };
    free(st);
}

// Like /device/status without the OpenThread read: br_core_get_status_brief. The Thread figures come from the collector.
static esp_err_t metrics_get_handler(httpd_req_t *req)
{
    br_core_status_t bs;
    builtin_rcp_status_t rs;
    sniffer_stats_t ss;

    br_core_get_status_brief(&bs);
    builtin_rcp_get_status(&rs);
    sniffer_get_stats(&ss);
    esp_netif_t *netif = mgmt_netif();
    metrics_device_t device = {
        .mode = bs.mode,
        .version = esp_app_get_description()->version,
        .slot = running_slot(),
        .rcp_version = rs.installed_known ? rs.installed : "",
        .rcp_compatible = bs.rcp_compatible,
        .rcp_compatible_known = bs.rcp_compatible_known,
        .storage_ok = !bs.storage_degraded,
        .sniffer_active = ss.active,
        .sniffer_frames = ss.frames,
        .sniffer_drops = ss.drops,
        .eth_up = netif && esp_netif_is_netif_up(netif),
        .trial_pending = netcfg_trial_pending(),
    };

    fill_zigbee_metrics(&device);
    httpd_resp_set_type(req, METRICS_CONTENT_TYPE);
    esp_err_t err = metrics_write(&device, send_metrics_chunk, req);

    if (err == ESP_ERR_NO_MEM) { // nothing was sent yet
        return send_error(req, 500, "no_memory", "out of memory", false);
    }
    if (err == ESP_OK) {
        err = httpd_resp_send_chunk(req, NULL, 0);
    }
    return err == ESP_OK ? ESP_OK : ESP_FAIL;
}

/* ---- POST/GET /device/topology ---- */

// Ends a running scan before OpenThread or the radio is taken. Without the OpenThread lock the scan still ends
// (stopped), but its mesh-diag operation runs on in OpenThread until it times out: log it and go on.
static void cancel_scan(void)
{
    if (mesh_scan_cancel() == ESP_ERR_TIMEOUT) {
        ESP_LOGW(TAG, "scan stopped without cancelling it in OpenThread (lock busy)");
    }
}

static esp_err_t send_topology_state(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = mesh_scan_render(send_metrics_chunk, req); // streams with httpd_resp_send_chunk

    if (err == ESP_OK) {
        err = httpd_resp_send_chunk(req, NULL, 0);
    }
    return err == ESP_OK ? ESP_OK : ESP_FAIL;
}

static esp_err_t topology_get_handler(httpd_req_t *req)
{
    return send_topology_state(req);
}

// Checks in order: the mode, the long-operation slot and the pcap slot (both read, not taken), then the scan start.
static esp_err_t topology_post_handler(httpd_req_t *req)
{
    br_core_mode_t mode = br_core_get_mode();
    const bool long_busy = device_api_op_long_running();
    const bool pcap_busy = device_api_op_pcap_running();
    device_api_topology_reply_t reply;
    esp_err_t start_err = ESP_OK;

    if (mode == BR_MODE_BR_RUNNING && !long_busy && !pcap_busy) {
        start_err = mesh_scan_start();
        if (start_err == ESP_ERR_NOT_ALLOWED) {
            mode = br_core_get_mode(); // the mode left BR_RUNNING meanwhile: name the one it is in now
        }
    }
    device_api_topology_start_reply(start_err, mode, long_busy, device_api_op_busy_detail(DEVICE_API_OP_OTA),
                                    pcap_busy, device_api_op_busy_detail(DEVICE_API_OP_PCAP), &reply);
    if (reply.error) {
        return send_error(req, reply.status, reply.error, reply.detail, false);
    }
    httpd_resp_set_status(req, device_api_status_line(reply.status));
    return send_topology_state(req);
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
    cancel_scan(); // a running scan must not hold OpenThread while the sniffer takes the radio
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
        esp_http_client_config_t config = self_update_http_config(job->url, OTA_URL_TIMEOUT_MS);
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
    const char *url; // the address asked for: a redirect leaves the client on a signed URL that expires
    int64_t length;
    size_t got;
} url_source_t;

// Open the connection and read the response headers, following redirects (a release asset always redirects).
// ESP_ERR_NOT_FOUND = reachable but not a usable 200.
static esp_err_t url_source_open(url_source_t *src)
{
    int status = 0;
    const esp_err_t err = self_update_http_open(src->client, &status);

    if (err != ESP_OK) {
        esp_http_client_close(src->client);
        ESP_LOGE(TAG, "connecting failed: %s", esp_err_to_name(err));
        return err;
    }
    const int64_t length = esp_http_client_get_content_length(src->client);

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
    const esp_err_t err = esp_http_client_set_url(src->client, src->url);
    if (err != ESP_OK) {
        return err;
    }
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

// Reply for a url_source_open failure; nothing was touched then.
static device_api_error_t map_url_source_error(esp_err_t err)
{
    if (err == DEVICE_API_ERR_TOO_LARGE) {
        return (device_api_error_t) {
            413, "payload_too_large", "the image is larger than the C6 slot"
        };
    }
    if (err == ESP_ERR_INVALID_SIZE) {
        return (device_api_error_t) {
            400, "length_required", "the URL gives no Content-Length"
        };
    }
    if (err == ESP_ERR_NO_MEM) {
        return (device_api_error_t) {
            500, "no_memory", "out of memory"
        };
    }
    return (device_api_error_t) {
        502, "fetch_failed", err == ESP_ERR_NOT_FOUND ? "the URL did not answer 200" : "the URL is not reachable"
    };
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
    builtin_rcp_status_t rs;
    char body[96];

    builtin_rcp_get_status(&rs);
    if (device_api_installed_json(body, sizeof(body), rs.installed_known, rs.installed) < 0) {
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
        const esp_http_client_config_t config = self_update_http_config(job->url, HTTP_CLIENT_TIMEOUT_MS);

        url_src.client = esp_http_client_init(&config);
        url_src.url = job->url;
        err = url_src.client ? url_source_open(&url_src) : ESP_ERR_NO_MEM;
        if (err != ESP_OK) {
            // Nothing was touched: the border router keeps running.
            const device_api_error_t error = map_url_source_error(err);

            send_mapped_error(req, error, error.detail, false);
            goto done;
        }
        source = (rcp_fw_source_t) {
            .total_size = (size_t)url_src.length, .read = url_source_read, .reopen = url_source_reopen, .ctx = &url_src,
        };
    }
    cancel_scan(); // the push tears OpenThread down
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
    // The board mode refuses only a job that stops OpenThread: with Thread on another channel it is that channel's.
    const char *mode_busy = device_api_radio_job_busy(br_core_get_mode(), br_core_c6_job_touches_openthread());

    if (mode_busy) {
        return send_error(req, 409, "busy", mode_busy, upload);
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
    cancel_scan(); // the recovery tears OpenThread down
    const esp_err_t err = br_core_rom_recover(detail, sizeof(detail));
    br_core_status_t bs;
    builtin_rcp_status_t rs;
    char body[384];
    int http_status = 0;

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ROM recovery failed: %s (%s)", esp_err_to_name(err), detail);
    }
    br_core_get_status(&bs); // the state after the restart: BR_RUNNING with the RCP, or RADIO_FAULT "no rcp firmware"
    builtin_rcp_get_status(&rs);
    // With Thread on another channel the mode and its fault are that channel's: the reply describes the C6 alone.
    const bool thread_on_c6 = strcmp(bs.otbr_channel, RADIO_MUX_C6_ID) == 0;

    if (device_api_recover_reply(err, detail[0] ? detail : esp_err_to_name(err), bs.mode, thread_on_c6, bs.fault_reason,
                                 rs.installed_known, rs.installed, body, sizeof(body), &http_status) < 0) {
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
    const char *mode_busy = device_api_radio_job_busy(br_core_get_mode(), br_core_c6_job_touches_openthread());

    if (mode_busy) {
        return send_error(req, 409, "busy", mode_busy, false);
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

/* ---- GET/POST /device/update (self-update from the GitHub releases) ---- */

#define UPDATE_BODY_MAX 1536
#define UPDATE_REPLY_MAX 768
#define UPDATE_DETAIL_MAX (URL_MAX + 64) // a reason and the URL asked for
#define UPDATE_FLAG_MAX 8

// A query flag: absent or `off` false, `on` true. False for anything else (the caller replies 400).
static bool query_flag(httpd_req_t *req, const char *key, const char *on, const char *off, bool *value)
{
    char *query = request_query(req);
    char text[UPDATE_FLAG_MAX];
    const esp_err_t err = device_api_query_value(query, key, text, sizeof(text));

    free(query);
    if (err == ESP_ERR_NOT_FOUND) {
        *value = false;
        return true;
    }
    if (err != ESP_OK) {
        return false;
    }
    *value = strcmp(text, on) == 0;
    return *value || strcmp(text, off) == 0;
}

static esp_err_t send_update_state(httpd_req_t *req)
{
    char *body = malloc(UPDATE_BODY_MAX);
    self_update_snapshot_t snap;
    builtin_rcp_status_t rs;
    esp_err_t err;

    if (!body) {
        return send_error(req, 500, "no_memory", "out of memory", false);
    }
    self_update_get(&snap);
    builtin_rcp_get_status(&rs);
    if (device_api_update_json(body, UPDATE_BODY_MAX, &snap, self_update_base(), esp_app_get_description()->version,
                               rs.installed_known, rs.installed) < 0) {
        err = send_error(req, 500, "internal", "update state does not fit its buffer", false);
    } else {
        err = send_json(req, body);
    }
    free(body);
    return err;
}

static void release_check_slot(void *ctx)
{
    (void)ctx;
    device_api_op_end(DEVICE_API_OP_UPDATE_CHECK);
}

// Waits for the handler's go (notification value 1: self_update_check_begin succeeded), runs the check, frees the slot.
// Order: the slot is freed after the fetch but BEFORE the result is published, while the state still reads checking.
// A check=true that arrives before the publish joins the running check; one that arrives after it finds the slot free
// and starts a new check. With the old order (publish, then free) a request between the two got 409 busy.
static void update_check_task(void *arg)
{
    uint32_t begun = 0;

    xTaskNotifyWait(0, UINT32_MAX, &begun, portMAX_DELAY);
    if (begun) {
        self_update_check_run_then(release_check_slot, NULL);
    } else {
        device_api_op_end(DEVICE_API_OP_UPDATE_CHECK);
    }
#ifdef OTBR_TEST_BUILD
    ESP_LOGI(TAG, "update check worker stack high water mark: %u of %d bytes free",
             (unsigned)uxTaskGetStackHighWaterMark(NULL), CHECK_STACK);
#endif
    vTaskDelete(NULL);
}

// Start a check on a worker, or join the one that runs. ESP_ERR_INVALID_STATE: another long operation holds the slot.
// The worker exists before the state moves to checking, so a failed task creation never leaves the cache "checking".
static esp_err_t start_update_check(void)
{
    self_update_snapshot_t snap;
    TaskHandle_t task = NULL;

    self_update_get(&snap);
    if (snap.state == SELF_UPDATE_CHECKING) {
        return ESP_OK; // join: no slot, no second fetch
    }
    if (!device_api_op_try_begin(DEVICE_API_OP_UPDATE_CHECK)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xTaskCreatePinnedToCore(update_check_task, "update_check", CHECK_STACK, NULL, WORKER_PRIORITY, &task,
                                WORKER_CORE) != pdPASS) {
        device_api_op_end(DEVICE_API_OP_UPDATE_CHECK);
        return ESP_ERR_NO_MEM;
    }
    // Only holders of the slot begin a check, so this cannot fail; if it did, the worker just frees the slot.
    xTaskNotify(task, self_update_check_begin() ? 1 : 0, eSetValueWithOverwrite);
    return ESP_OK;
}

static esp_err_t update_get_handler(httpd_req_t *req)
{
    bool check;

    if (!query_flag(req, "check", "true", "false", &check)) {
        return send_error(req, 400, "invalid_request", "check must be true or false", false);
    }
    if (check) {
        const esp_err_t err = start_update_check();

        if (err == ESP_ERR_INVALID_STATE) {
            return send_error(req, 409, "busy", device_api_op_busy_detail(DEVICE_API_OP_UPDATE_CHECK), false);
        }
        if (err != ESP_OK) {
            return send_error(req, 500, "no_memory", "cannot start the update check task", false);
        }
    }
    return send_update_state(req);
}

typedef struct {
    httpd_req_t *req; // async copy
    bool force;
    self_update_release_t release;
    char url[URL_MAX + 1]; // the asset being downloaded; esp_br_http_ota reads it while it runs
    char detail[UPDATE_DETAIL_MAX];
    char body[UPDATE_REPLY_MAX];
} update_job_t;

// Bundle step: the release's otbr.bundle through esp_br_http_ota (which follows the redirect). On failure the reply
// error is in *error and job->detail names the URL.
static esp_err_t update_bundle(update_job_t *job, device_api_error_t *error)
{
    esp_err_t err = self_update_asset_url(self_update_base(), job->release.tag, "otbr.bundle", job->url, sizeof(job->url));
    const bool url_failed = err != ESP_OK;

    if (!url_failed) {
        esp_http_client_config_t config = self_update_http_config(job->url, OTA_URL_TIMEOUT_MS);

        err = esp_br_http_ota(&config);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "update: bundle failed: %s (%s)", esp_err_to_name(err), job->url);
        // POST /device/update takes no arguments, so the OTA mapping's 400s do not fit: an asset URL that does not
        // fit its buffer is our fault (500), a redirect without Location is the server's (502).
        if (url_failed) {
            *error = (device_api_error_t) {
                500, "internal", NULL
            };
        } else if (err == ESP_ERR_INVALID_ARG) {
            *error = (device_api_error_t) {
                502, "transfer_failed", NULL
            };
        } else {
            *error = device_api_map_ota_error(err);
        }
        snprintf(job->detail, sizeof(job->detail), "%s (%s)", error->detail ? error->detail : esp_err_to_name(err),
                 job->url);
    }
    return err;
}

// RCP step: the release's c6_rcp.bin through url_source_* and br_core_push_radio_firmware. On failure the reply error
// (as POST /device/radio/firmware?from_url gives it) is in *error and job->detail says why.
static esp_err_t update_rcp(update_job_t *job, device_api_error_t *error)
{
    url_source_t src = {0};
    esp_err_t err = self_update_asset_url(self_update_base(), job->release.tag, "c6_rcp.bin", job->url, sizeof(job->url));

    if (err != ESP_OK) {
        *error = (device_api_error_t) {
            500, "internal", NULL
        };
        strlcpy(job->detail, "the radio firmware URL does not fit its buffer", sizeof(job->detail));
        return err;
    }
    const esp_http_client_config_t config = self_update_http_config(job->url, HTTP_CLIENT_TIMEOUT_MS);

    src.client = esp_http_client_init(&config);
    src.url = job->url;
    err = src.client ? url_source_open(&src) : ESP_ERR_NO_MEM;
    if (err != ESP_OK) {
        *error = map_url_source_error(err);
        snprintf(job->detail, sizeof(job->detail), "%s (%s)", error->detail, job->url);
    } else {
        const rcp_fw_source_t source = {
            .total_size = (size_t)src.length, .read = url_source_read, .reopen = url_source_reopen, .ctx = &src,
        };

        job->detail[0] = '\0';
        cancel_scan(); // the push tears OpenThread down
        err = br_core_push_radio_firmware(&source, job->detail, sizeof(job->detail));
        if (err != ESP_OK) {
            *error = device_api_map_push_error(err);
            if (job->detail[0] == '\0') {
                strlcpy(job->detail, error->detail ? error->detail : esp_err_to_name(err), sizeof(job->detail));
            }
        }
    }
    if (src.client) {
        esp_http_client_cleanup(src.client);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "update: radio firmware failed: %s (%s)", esp_err_to_name(err), job->detail);
    }
    return err;
}

// The install: a fresh check, the decision, the bundle, then the radio firmware (design decision 4). Sends the reply;
// returns true when the bundle was committed (the caller reboots, the slot stays taken).
static bool run_update(update_job_t *job)
{
    httpd_req_t *req = job->req;
    const self_update_release_t *rel = &job->release;
    device_api_error_t error;
    builtin_rcp_status_t rs;
    bool install_app;
    bool install_rcp;

    if (self_update_fetch_release(&job->release, job->detail, sizeof(job->detail)) != ESP_OK) {
        send_error(req, 502, "check_failed", job->detail, false);
        return false;
    }
    builtin_rcp_get_status(&rs);
    error = device_api_update_decide(self_update_compare(rel->app, esp_app_get_description()->version),
                                     self_update_rcp_differs(rel->rcp, rs.installed, rs.installed_known),
                                     rel->rcp[0] != '\0', job->force, &install_app, &install_rcp);
    if (error.http_status != 0) {
        send_mapped_error(req, error, error.detail, false);
        return false;
    }
    const char *mode_busy = install_rcp ? device_api_radio_job_busy(br_core_get_mode(),
                                                                    br_core_c6_job_touches_openthread()) : NULL;

    if (mode_busy) {
        // Before the bundle: the push would be refused, and nothing is changed yet.
        send_error(req, 409, "busy", mode_busy, false);
        return false;
    }
    ESP_LOGI(TAG, "update to %s: app %s, radio firmware %s", rel->tag[0] ? rel->tag : "the latest release",
             install_app ? "yes" : "no", install_rcp ? "yes" : "no");
    if (install_app && update_bundle(job, &error) != ESP_OK) {
        send_mapped_error(req, error, job->detail, false); // nothing was changed
        return false;
    }
    const char *rcp_result = "unchanged";

    if (install_rcp) {
        if (update_rcp(job, &error) == ESP_OK) {
            rcp_result = "installed";
        } else if (!install_app) {
            send_mapped_error(req, error, job->detail, false); // a failed push leaves the old radio firmware running
            return false;
        } else {
            rcp_result = "failed"; // the committed bundle still boots; the next update pushes the radio firmware only
        }
    }
    // A failed radio step keeps its reason in job->detail (the page shows it after the reboot).
    if (!install_app) {
        strlcpy(job->detail, "radio firmware installed", sizeof(job->detail));
    } else if (strcmp(rcp_result, "failed") != 0) {
        strlcpy(job->detail, install_rcp ? "update and radio firmware installed, rebooting" : "update installed, rebooting",
                sizeof(job->detail));
    }
    if (device_api_update_reply_json(job->body, sizeof(job->body), install_app, rcp_result, job->detail, install_app) < 0) {
        send_error(req, 500, "internal", "reply does not fit its buffer", false);
    } else {
        send_json(req, job->body);
    }
    return install_app;
}

static void update_task(void *arg)
{
    update_job_t *job = arg;
    httpd_req_t *req = job->req;
    sock_timeouts_t saved;

    set_socket_timeouts(req, LONG_RECV_TIMEOUT_S, SEND_TIMEOUT_S, &saved);
    const bool rebooting = run_update(job);

    if (rebooting) {
        finish_request(req, &saved);
        schedule_reboot(REBOOT_DELAY_MS); // the slot stays taken: the update waits for its reboot
    } else {
        // Free the slot before the reply goes out: a page that sends GET ?check=true on the reply must not get 409.
        device_api_op_end(DEVICE_API_OP_OTA);
        finish_request(req, &saved);
    }
#ifdef OTBR_TEST_BUILD
    ESP_LOGI(TAG, "update worker stack high water mark: %u of %d bytes free",
             (unsigned)uxTaskGetStackHighWaterMark(NULL), UPDATE_STACK);
#endif
    free(job);
    vTaskDelete(NULL);
}

// Held until the install finishes, like POST /device/ota/url. The OTA slot covers both steps.
static esp_err_t update_post_handler(httpd_req_t *req)
{
    bool force;

    if (req->content_len != 0) {
        return send_error(req, 400, "invalid_request", "the request takes no body", true);
    }
    if (!query_flag(req, "force", "1", "0", &force)) {
        return send_error(req, 400, "invalid_request", "force must be 0 or 1", false);
    }
    if (!device_api_op_try_begin(DEVICE_API_OP_OTA)) {
        return send_error(req, 409, "busy", device_api_op_busy_detail(DEVICE_API_OP_OTA), false);
    }
    update_job_t *job = calloc(1, sizeof(*job));
    httpd_req_t *async_req = NULL;

    if (!job || httpd_req_async_handler_begin(req, &async_req) != ESP_OK) {
        free(job);
        device_api_op_end(DEVICE_API_OP_OTA);
        return send_error(req, 500, "internal", "cannot start the update", false);
    }
    job->req = async_req;
    job->force = force;
    if (xTaskCreatePinnedToCore(update_task, "update", UPDATE_STACK, job, WORKER_PRIORITY, NULL, WORKER_CORE) != pdPASS) {
        send_error(async_req, 500, "no_memory", "cannot start the update task", false);
        httpd_req_async_handler_complete(async_req);
        free(job);
        device_api_op_end(DEVICE_API_OP_OTA);
    }
    return ESP_OK;
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

// A change of `trel` alone is applied live: stored in the active config (no trial, no reboot) and handed to OpenThread.
static esp_err_t network_put_trel(httpd_req_t *req, bool enabled)
{
    if (netcfg_trial_pending()) {
        return send_error(req, 409, "busy", "confirm or revert the network change first", true);
    }
    esp_err_t err = netcfg_set_trel(enabled);

    if (err != ESP_OK) {
        const device_api_error_t error = device_api_map_network_set_error(err);

        return send_mapped_error(req, error, error.detail, false);
    }
    err = br_core_set_trel(enabled);
    if (err == ESP_ERR_TIMEOUT) {
        return send_json(req, "{\"status\":\"saved\",\"detail\":\"OpenThread is busy: the setting applies at its next start\"}");
    }
    if (err == ESP_ERR_INVALID_STATE) {
        return send_json(req, "{\"status\":\"saved\",\"detail\":\"Thread is not running: the setting applies when it starts\"}");
    }
    if (err == ESP_ERR_NOT_SUPPORTED) {
        return send_json(req, "{\"status\":\"saved\",\"detail\":\"this firmware is built without TREL\"}");
    }
    return send_json(req, "{\"status\":\"applied\"}");
}

static esp_err_t network_put_handler(httpd_req_t *req)
{
    const int length_status = device_api_length_check(req->content_len, NETWORK_BODY_MAX);

    if (length_status != 0) {
        return send_error(req, length_status, length_status == 413 ? "payload_too_large" : "length_required",
                          "send the network configuration as a JSON body", true);
    }
    char *body = read_small_body(req);

    if (!body) {
        return send_error(req, 400, "invalid_request", "the body could not be read", true);
    }
    netcfg_config_t running, cfg;
    const char *bad_field = NULL;
    esp_err_t err = netcfg_get_running(&running);

    if (err == ESP_OK) {
        err = netcfg_from_json_over(body, &running, &cfg, &bad_field);
    }
    free(body);
    if (err == ESP_OK && netcfg_diff_only_trel(&running, &cfg)) {
        return network_put_trel(req, cfg.trel);
    }
    // A reboot into the new config would cut an update or a firmware push short.
    if (err == ESP_OK && device_api_op_long_running()) {
        return send_error(req, 409, "busy", "an update or firmware push is running or waits for its reboot", true);
    }
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

/* ---- GET /device/zigbee ---- */

// 409 while no channel is in mode ziggurat.
static esp_err_t zigbee_get_handler(httpd_req_t *req)
{
    if (!zigbee_channel()) {
        return send_error(req, 409, "not_zigbee", "no channel is in mode ziggurat", false);
    }
    zigbee_status_t *st = malloc(sizeof(*st));
    char *body = malloc(ZIGBEE_JSON_MAX);
    esp_err_t err;
    size_t len;

    if (!st || !body) {
        err = send_error(req, 500, "no_memory", "out of memory", false);
    } else {
        zigbee_get_status(st);
        if (zigbee_json_status(st, body, ZIGBEE_JSON_MAX, &len) != ESP_OK) {
            err = send_error(req, 500, "internal", "the status does not fit its buffer", false);
        } else {
            err = send_json(req, body);
        }
    }
    free(st);
    free(body);
    return err;
}

/* ---- /device/names ---- */

#define NAMES_BODY_MAX 512
#define NAMES_JSON_MAX 1024

static esp_err_t send_names(httpd_req_t *req, const netnames_result_t *res)
{
    netnames_t names;
    char body[NAMES_JSON_MAX];

    netnames_get(&names);
    if (device_api_names_json(body, sizeof(body), &names, netcfg_running_mode() == NETCFG_MODE_SPLIT, res) < 0) {
        return send_error(req, 500, "internal", "the names do not fit the reply", false);
    }
    return send_json(req, body);
}

static esp_err_t names_get_handler(httpd_req_t *req)
{
    return send_names(req, NULL);
}

static esp_err_t names_put_handler(httpd_req_t *req)
{
    const int length_status = device_api_length_check(req->content_len, NAMES_BODY_MAX);

    if (length_status != 0) {
        return send_error(req, length_status, length_status == 413 ? "payload_too_large" : "length_required",
                          "send the names as a JSON body", true);
    }
    char *body = read_small_body(req);

    if (!body) {
        return send_error(req, 400, "invalid_request", "the body could not be read", true);
    }
    netnames_settings_t settings;
    netnames_result_t res = {0};
    const char *field = NULL;
    esp_err_t err = device_api_names_parse(body, &settings, &field);

    free(body);
    if (err != ESP_OK) {
        res.field = field;
        res.reason = "missing, of the wrong type or longer than 63 bytes";
    } else {
        err = netnames_set(&settings, &res);
    }
    if (err == ESP_ERR_INVALID_ARG) {
        char error[256];

        if (device_api_names_error_json(error, sizeof(error), res.field, res.reason) < 0) {
            return send_error(req, 400, "invalid_config", res.field, false);
        }
        httpd_resp_set_status(req, device_api_status_line(400));
        return send_json(req, error);
    }
    if (err != ESP_OK) {
        return send_error(req, 500, "storage_failed", "the names could not be stored", false);
    }
    return send_names(req, &res);
}

/* ---- /device/console (Thread console spec) ---- */

#define CONSOLE_BODY_MAX 64

static esp_err_t send_console(httpd_req_t *req)
{
    char peer[48];
    uint32_t since_s = 0;
    const bool attached = ot_console_session(peer, sizeof(peer), &since_s);
    char body[160];

    if (device_api_console_json(body, sizeof(body), ot_console_enabled(), attached ? peer : NULL, since_s) < 0) {
        return send_error(req, 500, "internal", "the reply does not fit its buffer", false);
    }
    return send_json(req, body);
}

static esp_err_t console_get_handler(httpd_req_t *req)
{
    return send_console(req);
}

static esp_err_t console_put_handler(httpd_req_t *req)
{
    const int length_status = device_api_length_check(req->content_len, CONSOLE_BODY_MAX);

    if (length_status != 0) {
        return send_error(req, length_status, length_status == 413 ? "payload_too_large" : "length_required",
                          "send {\"enabled\": true|false}", true);
    }
    char *body = read_small_body(req);

    if (!body) {
        return send_error(req, 400, "invalid_request", "the body could not be read", true);
    }
    bool enabled = false;
    const esp_err_t parse_err = device_api_console_parse(body, &enabled);

    free(body);
    if (parse_err != ESP_OK) {
        return send_error(req, 400, "invalid_request", "\"enabled\" must be true or false", false);
    }
    const esp_err_t set_err = ot_console_set_enabled(enabled);

    if (set_err == ESP_ERR_INVALID_STATE) {
        return send_error(req, 503, "not_started", "the Thread console did not start, see the log", false);
    }
    if (set_err != ESP_OK) {
        return send_error(req, 500, "storage_failed", "the console setting could not be stored", false);
    }
    return send_console(req);
}

/* ---- /device/routing (mesh routing spec section 6) ---- */

#define ROUTING_BODY_MAX 2048

// 200 with the GET /device/routing shape: the stored settings and the task's last status.
static esp_err_t send_routing_state(httpd_req_t *req)
{
    routing_settings_t settings;
    routing_status_t status;

    if (routing_get(&settings, &status) != ESP_OK) {
        return send_error(req, 503, "not_started", "routing is not running", false);
    }
    char *json = routing_settings_to_json(&settings, &status);

    if (!json) {
        return send_error(req, 500, "no_memory", "out of memory", false);
    }
    const esp_err_t err = send_json(req, json);

    free(json);
    return err;
}

static esp_err_t routing_get_handler(httpd_req_t *req)
{
    return send_routing_state(req);
}

// Validates the body against the current settings and the backbone, stores it and has the routing task apply it live
// (no reboot, no trial); a failed check changes nothing.
static esp_err_t routing_put_handler(httpd_req_t *req)
{
    const int length_status = device_api_length_check(req->content_len, ROUTING_BODY_MAX);

    if (length_status != 0) {
        return send_error(req, length_status, length_status == 413 ? "payload_too_large" : "length_required",
                          "send the routing settings as a JSON body", true);
    }
    char *body = read_small_body(req);

    if (!body) {
        return send_error(req, 400, "invalid_request", "the body could not be read", true);
    }
    routing_settings_t current;

    if (routing_get(&current, NULL) != ESP_OK) {
        free(body);
        return send_error(req, 503, "not_started", "routing is not running", false);
    }
    // A busy OpenThread lock leaves out only the on-link prefixes of received RAs; the rest is still checked.
    routing_validate_ctx_t ctx;
    const esp_err_t ctx_err = routing_validate_ctx_fill(&ctx);

    if (ctx_err != ESP_OK) {
        ESP_LOGW(TAG, "routing validation without the RA prefixes (%s)", esp_err_to_name(ctx_err));
    }
    routing_settings_t next;
    const char *bad_field = NULL;
    esp_err_t err = routing_settings_from_json(body, &current, &ctx, &next, &bad_field);

    free(body);
    if (err == ESP_ERR_INVALID_ARG) {
        return send_error(req, 400, "invalid_request", bad_field ? bad_field : "body", false);
    }
    if (err != ESP_OK) {
        return send_error(req, 500, "internal", esp_err_to_name(err), false);
    }
    err = routing_apply(&next);
    if (err == ESP_ERR_INVALID_STATE) {
        return send_error(req, 503, "not_started", "routing is not running", false);
    }
    if (err != ESP_OK) {
        char detail[80];

        snprintf(detail, sizeof(detail), "storing the routing settings failed (%s); nothing changed",
                 esp_err_to_name(err));
        return send_error(req, 500, "storage_failed", detail, false);
    }
    return send_routing_state(req);
}

/* ---- /device/thread ---- */

#define THREAD_BODY_MAX 256
#define THREAD_JSON_MAX 256

// The 400 reply of a body the parser refused, built like the /device/names one.
static esp_err_t send_thread_invalid(httpd_req_t *req, const char *field)
{
    char error[256];
    const char *reason = "a JSON object with leader_weight_adjustment and/or become_leader"; // field "body"

    if (strcmp(field, "leader_weight_adjustment") == 0) {
        reason = "an integer from -16 to 16";
    } else if (strcmp(field, "become_leader") == 0) {
        reason = "true or false";
    }
    if (device_api_names_error_json(error, sizeof(error), field, reason) < 0) {
        return send_error(req, 400, "invalid_config", field, false);
    }
    httpd_resp_set_status(req, device_api_status_line(400));
    return send_json(req, error);
}

static esp_err_t thread_get_handler(httpd_req_t *req)
{
    device_api_thread_t t = {.adjustment = br_core_leader_adjustment()};
    char body[THREAD_JSON_MAX];
    const esp_err_t err = br_core_leader_weight(&t.weight);

    t.weight_known = err == ESP_OK;
    t.partial = err == ESP_ERR_TIMEOUT;
    if (device_api_thread_json(body, sizeof(body), &t) < 0) {
        return send_error(req, 500, "internal", "the reply does not fit its buffer", false);
    }
    return send_json(req, body);
}

// An error of the PUT; after a stored adjustment (t->has_applied) it carries the adjustment and `applied` too.
static esp_err_t send_thread_error(httpd_req_t *req, int http_status, const char *code, const char *detail,
                                   const device_api_thread_t *t)
{
    char body[384];

    if (device_api_thread_error_json(body, sizeof(body), code, detail, t) < 0) {
        return send_error(req, http_status, code, detail, false);
    }
    httpd_resp_set_status(req, device_api_status_line(http_status));
    return send_json(req, body);
}

static esp_err_t thread_put_handler(httpd_req_t *req)
{
    const int length_status = device_api_length_check(req->content_len, THREAD_BODY_MAX);

    if (length_status != 0) {
        return send_error(req, length_status, length_status == 413 ? "payload_too_large" : "length_required",
                          "send the leader weight settings as a JSON body", true);
    }
    char *raw = read_small_body(req);

    if (!raw) {
        return send_error(req, 400, "invalid_request", "the body could not be read", true);
    }
    device_api_thread_put_t put;
    const char *field = NULL;
    const esp_err_t parse_err = device_api_thread_parse(raw, &put, &field);

    free(raw);
    if (parse_err != ESP_OK) {
        return send_thread_invalid(req, field);
    }
    device_api_thread_t t = {0};

    if (put.has_adjustment) {
        br_core_leader_apply_t applied = BR_CORE_LEADER_NOT_READY;
        const esp_err_t err = br_core_set_leader_adjustment(put.adjustment, &applied);

        if (err == ESP_ERR_INVALID_ARG) { // the parser checks the range; cannot happen
            return send_thread_invalid(req, "leader_weight_adjustment");
        }
        if (err != ESP_OK) { // ESP_ERR_INVALID_STATE (storage degraded) or an NVS error
            return send_error(req, 500, "storage_failed", "the leader weight adjustment could not be stored", false);
        }
        t.has_applied = true;
        t.adjustment = put.adjustment; // for an error reply of the take-over; the final reply re-reads it
        t.applied = applied == BR_CORE_LEADER_APPLIED;
        if (applied == BR_CORE_LEADER_NOT_READY) {
            t.detail = "Thread is not running";
        } else if (applied == BR_CORE_LEADER_BUSY) {
            t.detail = "OpenThread is busy, applied within seconds or at the next start";
        }
    }
    uint8_t started_weight = 0;

    if (put.become_leader) {
        br_core_leader_refusal_t refusal = BR_CORE_LEADER_OK;
        const esp_err_t err = br_core_become_leader(&refusal, &started_weight);

        if (err == ESP_ERR_INVALID_STATE) {
            int status;
            const char *code;
            char detail[64];

            device_api_thread_refusal(refusal, br_core_get_mode(), &status, &code, detail, sizeof(detail));
            return send_thread_error(req, status, code, detail, &t);
        }
        if (err == ESP_ERR_NOT_FINISHED) {
            return send_thread_error(req, 503, "busy", "OpenThread is restarting, retry", &t);
        }
        if (err == ESP_ERR_TIMEOUT) {
            return send_thread_error(req, 503, "busy", "OpenThread is busy, retry", &t);
        }
        if (err != ESP_OK) {
            return send_thread_error(req, 500, "internal", "taking over failed", &t);
        }
        t.become_started = true;
    }
    t.adjustment = br_core_leader_adjustment();
    const esp_err_t weight_err = br_core_leader_weight(&t.weight);
    char body[THREAD_JSON_MAX];

    t.weight_known = weight_err == ESP_OK;
    t.partial = weight_err == ESP_ERR_TIMEOUT;
    if (!t.weight_known && t.become_started) { // the weight the take-over read under its own lock
        t.weight = started_weight;
        t.weight_known = true;
        t.partial = false;
    }
    if (device_api_thread_json(body, sizeof(body), &t) < 0) {
        return send_error(req, 500, "internal", "the reply does not fit its buffer", false);
    }
    return send_json(req, body);
}

/* ---- /device/channels (spec 2026-11-06 radio channel mux, section API) ---- */

#define CHANNELS_PREFIX "/device/channels/"

// The OTBR channel's id ("" for none), copied out of the mux.
static void otbr_channel_id(char out[RADIO_MUX_ID_MAX])
{
    radio_mux_otbr_channel(out, RADIO_MUX_ID_MAX);
}

// What the mux leaves to device_api: the proxy's state, peer and last error of channels in rcp-proxy, and the board's
// view of the OTBR channel (OpenThread failed or still starting on a channel the mux lists running, a refused stop).
static void fill_channels(radio_mux_snapshot_t *s)
{
    char refused_id[RADIO_MUX_ID_MAX];
    char refused_reason[RADIO_MUX_REASON_MAX];
    br_core_status_t bs;

    for (size_t i = 0; i < s->n; i++) {
        radio_mux_chan_info_t *c = &s->chans[i];
        rcp_proxy_status_t ps;

        if (c->mode == RADIO_MUX_MODE_RCP_PROXY && rcp_proxy_get_status(c->id, &ps) == ESP_OK) {
            c->proxy_state = ps.state; // a static string
            strlcpy(c->proxy_peer, ps.peer, sizeof(c->proxy_peer));
            strlcpy(c->proxy_last_error, ps.last_error, sizeof(c->proxy_last_error));
        }
    }
    br_core_get_status_brief(&bs); // mode, fault reason and the otbr channel; never the OpenThread lock
    const bool refused = br_core_otbr_refusal(refused_id, sizeof(refused_id), refused_reason, sizeof(refused_reason));
    const device_api_board_view_t b = {
        .otbr_id = bs.otbr_channel[0] ? bs.otbr_channel : NULL,
        .mode = bs.mode,
        .fault_reason = bs.fault_reason,
        .moving = br_core_move_running(),
        .refused_id = refused ? refused_id : NULL,
        .refused_reason = refused_reason,
    };

    device_api_channels_overlay_board(s, &b);
}

static const radio_mux_chan_info_t *find_channel(const radio_mux_snapshot_t *s, const char *id)
{
    for (size_t i = 0; i < s->n; i++) {
        if (strcmp(s->chans[i].id, id) == 0) {
            return &s->chans[i];
        }
    }
    return NULL;
}

static esp_err_t channels_get_handler(httpd_req_t *req)
{
    radio_mux_snapshot_t *s = malloc(sizeof(*s));
    char *body = malloc(DEVICE_API_CHANNELS_BODY_MAX);
    esp_err_t err;

    if (!s || !body) {
        err = send_error(req, 500, "no_memory", "out of memory", false);
    } else {
        radio_mux_get_snapshot(s);
        fill_channels(s);
        err = device_api_channels_json(body, DEVICE_API_CHANNELS_BODY_MAX, s) < 0
              ? send_error(req, 500, "internal", "the reply does not fit its buffer", false) : send_json(req, body);
    }
    free(s);
    free(body);
    return err;
}

// The id after CHANNELS_PREFIX (the wildcard route), without a query; false when empty or longer than an id can be.
static bool channel_id_from_uri(httpd_req_t *req, char id[RADIO_MUX_ID_MAX])
{
    const char *p = req->uri + strlen(CHANNELS_PREFIX);
    const size_t n = strcspn(p, "?#");

    if (strncmp(req->uri, CHANNELS_PREFIX, strlen(CHANNELS_PREFIX)) != 0 || n == 0 || n >= RADIO_MUX_ID_MAX) {
        return false;
    }
    memcpy(id, p, n);
    id[n] = '\0';
    return true;
}

// The 200 reply: the channel as it is after the change (proxy and board state filled in), "reboot" when it reboots.
static esp_err_t send_channel(httpd_req_t *req, radio_mux_snapshot_t *s, const char *id, bool reboot)
{
    char *body = malloc(DEVICE_API_CHANNEL_BODY_MAX);
    esp_err_t err;

    radio_mux_get_snapshot(s);
    fill_channels(s);
    const radio_mux_chan_info_t *c = find_channel(s, id);

    if (!body) {
        err = send_error(req, 500, "no_memory", "out of memory", false);
    } else if (!c) { // forgotten in between: the change was made, but there is nothing to show
        err = send_error(req, 404, "unknown_channel", "no radio with this id", false);
    } else if (device_api_channel_json(body, DEVICE_API_CHANNEL_BODY_MAX, c, reboot) < 0) {
        err = send_error(req, 500, "internal", "the reply does not fit its buffer", false);
    } else {
        err = send_json(req, body);
    }
    free(body);
    return err;
}

// Validates against the stored settings, takes the long-operation slot for a move, applies through the mux, kicks the
// proxy's peer or retries OpenThread's start when asked, and replies. `s` is a scratch snapshot.
static esp_err_t channel_apply(httpd_req_t *req, radio_mux_snapshot_t *s, const char *id,
                               const radio_mux_set_req_t *set)
{
    char otbr[RADIO_MUX_ID_MAX];
    char moved_from[RADIO_MUX_ID_MAX] = "";
    char reason[RADIO_MUX_REASON_MAX] = "";

    radio_mux_get_snapshot(s);
    const radio_mux_chan_info_t *c = find_channel(s, id);

    if (!c) {
        return send_error(req, 404, "unknown_channel", "no radio with this id", false);
    }
    const char *bad = device_api_channel_proxy_refusal(&c->proxy, set);

    if (bad) {
        return send_error(req, 400, "bad_value", bad, false);
    }
    otbr_channel_id(otbr);
    const char *mixed = device_api_channel_move_refusal(set, id, otbr[0] ? otbr : NULL, s->reboot_modes, c->mode);

    if (mixed) { // before port_taken: settings that go with a start, move or stop are never stored
        return send_error(req, 400, "bad_value", mixed, false);
    }
    const char *taken_by = device_api_channel_port_conflict(s, id, set);

    if (taken_by) {
        char detail[RADIO_MUX_REASON_MAX + RADIO_MUX_ID_MAX];
        const radio_mux_proxy_settings_t *p = &c->proxy;
        const uint16_t port = set->has_proxy && (set->proxy_fields & RADIO_MUX_PROXY_F_PORT) ? set->proxy.port : p->port;

        snprintf(detail, sizeof(detail), "rcp_proxy.port %u is taken: %s listens on it", (unsigned)port, taken_by);
        return send_error(req, 409, "port_taken", detail, false);
    }
    const char *moving = device_api_channel_move_busy(set, br_core_move_running() ||
                                                      device_api_op_holds(DEVICE_API_OP_RADIO_MOVE));

    if (moving) {
        return send_error(req, 409, "busy", moving, false);
    }
    // A move is a long operation: it takes the shared slot before br_core is asked, and br_core gives it back once
    // the move ended (on_move_done). A refusal before the 202 gives it back at once.
    const bool move = device_api_channel_is_move(set, id, otbr[0] ? otbr : NULL, s->reboot_modes, c->mode);

    if (move && !device_api_op_try_begin(DEVICE_API_OP_RADIO_MOVE)) {
        return send_error(req, 409, "busy", device_api_op_busy_detail(DEVICE_API_OP_RADIO_MOVE), false);
    }
    const radio_mux_verdict_t v = radio_mux_set(id, set, moved_from, sizeof(moved_from), reason, sizeof(reason));

    if (move && v != RADIO_MUX_MOVE) {
        device_api_op_end(DEVICE_API_OP_RADIO_MOVE);
    } else if (!move && v == RADIO_MUX_MOVE) { // the OTBR channel changed under the request: runs without the slot
        ESP_LOGW(TAG, "%s: OpenThread moves without the long-operation slot", id);
    }
    const bool accepted = v == RADIO_MUX_APPLY || v == RADIO_MUX_APPLY_REBOOT || v == RADIO_MUX_MOVE;

    if (accepted && set->kick) {
        rcp_proxy_kick(id); // drops the current peer; never stored
    }
    if (device_api_channel_retry_otbr(set, v, id, otbr[0] ? otbr : NULL, br_core_get_mode())) {
        br_core_retry_otbr(); // a PUT of the mode retries a fault (spec: Board mode, faults and status)
    }
    int status;
    const char *code;

    device_api_channel_refusal(v, &status, &code);
    if (code) {
        return send_error(req, status, code, reason[0] ? reason : device_api_channel_refusal_detail(v), false);
    }
    if (v == RADIO_MUX_MOVE) {
        char body[96];

        if (device_api_channel_moving_json(body, sizeof(body), moved_from) < 0) {
            return send_error(req, 500, "internal", "the reply does not fit its buffer", false);
        }
        httpd_resp_set_status(req, device_api_status_line(202));
        return send_json(req, body);
    }
    const esp_err_t err = send_channel(req, s, id, v == RADIO_MUX_APPLY_REBOOT);

    if (v == RADIO_MUX_APPLY_REBOOT) {
        reboot_gate_restart(REBOOT_DELAY_MS); // stored: the mode's consumer takes the change at boot
    }
    return err;
}

static esp_err_t channel_put_handler(httpd_req_t *req)
{
    char id[RADIO_MUX_ID_MAX];
    radio_mux_set_req_t set;
    const char *bad = NULL;

    if (!channel_id_from_uri(req, id)) {
        return send_error(req, 404, "unknown_channel", "no radio with this id", true);
    }
    const int length_status = device_api_length_check(req->content_len, DEVICE_API_CHANNEL_PUT_MAX);

    if (length_status != 0) {
        return send_error(req, length_status, length_status == 413 ? "payload_too_large" : "length_required",
                          "send the change as a JSON body", true);
    }
    char *body = read_small_body(req);

    if (!body) {
        return send_error(req, 400, "invalid_request", "the body could not be read", true);
    }
    const esp_err_t parse_err = device_api_channel_put_parse(body, &set, &bad);

    free(body);
    if (parse_err != ESP_OK) {
        return send_error(req, 400, "bad_value", bad, false);
    }
    radio_mux_snapshot_t *s = malloc(sizeof(*s));

    if (!s) {
        return send_error(req, 500, "no_memory", "out of memory", false);
    }
    const esp_err_t err = channel_apply(req, s, id, &set);

    free(s);
    return err;
}

static esp_err_t channel_delete_handler(httpd_req_t *req)
{
    char id[RADIO_MUX_ID_MAX];
    const esp_err_t err = channel_id_from_uri(req, id) ? radio_mux_forget(id) : ESP_ERR_NOT_FOUND;
    const device_api_error_t e = device_api_channel_forget_error(err, radio_mux_started());

    if (e.http_status != 204) {
        return send_error(req, e.http_status, e.code, e.detail, false);
    }
    httpd_resp_set_status(req, device_api_status_line(204));
    return httpd_resp_send(req, NULL, 0);
}

// br_core's radio task: a move that answered 202 has ended; its long-operation slot is free again.
static void on_move_done(void)
{
    device_api_op_end(DEVICE_API_OP_RADIO_MOVE);
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

// A WebSocket route's policy check, before the handshake (its frames then need none). In NO_RADIO the console has no
// OpenThread to talk to: 503 as the Thread REST API answers.
static esp_err_t gated_ws_handshake(httpd_req_t *req)
{
    if (!netcfg_request_allowed(req, NETCFG_SVC_DEVICE_API)) {
        return ESP_FAIL;
    }
    const int refused = device_api_console_ws_refusal(br_core_get_mode());

    if (refused) {
        send_error(req, refused, "no_radio", DEVICE_API_NO_RADIO_DETAIL, true);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t device_api_register(httpd_handle_t server)
{
    ESP_RETURN_ON_FALSE(server != NULL, ESP_ERR_INVALID_ARG, TAG, "no server");
    if (!s_reboot_timer) { // once, before any handler can run
        const esp_timer_create_args_t args = {.callback = reboot_now, .name = "device_reboot"};
        ESP_RETURN_ON_ERROR(esp_timer_create(&args, &s_reboot_timer), TAG, "creating the reboot timer failed");
    }
    br_core_set_move_done_cb(on_move_done);
    const esp_err_t scan_err = mesh_scan_init();

    if (scan_err != ESP_OK) { // the rest of the device API does not depend on it
        ESP_LOGE(TAG, "mesh scan init failed: %s", esp_err_to_name(scan_err));
    }
    static const route_t routes[] = {
        {"/device/status", HTTP_GET, status_get_handler},
        {"/device/sniffer.pcap", HTTP_GET, pcap_get_handler},
        {"/device/sniffer", HTTP_DELETE, sniffer_delete_handler},
        {"/device/ota", HTTP_POST, ota_post_handler},
        {"/device/ota/url", HTTP_POST, ota_url_post_handler},
        {"/device/update", HTTP_GET, update_get_handler},
        {"/device/update", HTTP_POST, update_post_handler},
        {"/device/radio/firmware", HTTP_POST, radio_firmware_post_handler},
        {"/device/radio/recover", HTTP_POST, radio_recover_post_handler},
        {"/device/radio/log", HTTP_GET, radio_log_get_handler},
        {"/device/reboot", HTTP_POST, reboot_post_handler},
        {"/device/diag", HTTP_GET, device_api_diag_get_handler},
        {"/device/network", HTTP_GET, network_get_handler},
        {"/device/network", HTTP_PUT, network_put_handler},
        {"/device/network/confirm", HTTP_POST, network_confirm_post_handler},
        {"/device/zigbee", HTTP_GET, zigbee_get_handler},
        {"/device/names", HTTP_GET, names_get_handler},
        {"/device/names", HTTP_PUT, names_put_handler},
        {"/device/thread", HTTP_GET, thread_get_handler},
        {"/device/thread", HTTP_PUT, thread_put_handler},
        {"/device/topology", HTTP_POST, topology_post_handler},
        {"/device/topology", HTTP_GET, topology_get_handler},
        {"/device/routing", HTTP_GET, routing_get_handler},
        {"/device/routing", HTTP_PUT, routing_put_handler},
        {"/device/console", HTTP_GET, console_get_handler},
        {"/device/console", HTTP_PUT, console_put_handler},
        {"/device/channels", HTTP_GET, channels_get_handler},
        {CHANNELS_PREFIX "*", HTTP_PUT, channel_put_handler},
        {CHANNELS_PREFIX "*", HTTP_DELETE, channel_delete_handler},
        {"/metrics", HTTP_GET, metrics_get_handler},
    };

    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        const httpd_uri_t uri = {.uri = routes[i].uri, .method = routes[i].method, .handler = gated_handler, .user_ctx = (void *) &routes[i]};

        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &uri), TAG, "registering %s failed", routes[i].uri);
    }
    // The Thread console's WebSocket: the policy check runs before the upgrade, the handler gets each frame after it.
    const httpd_uri_t console_ws = {
        .uri = "/device/console/ws",
        .method = HTTP_GET,
        .handler = device_api_console_ws_frame,
        .is_websocket = true,
        .ws_pre_handshake_cb = gated_ws_handshake,
        .ws_post_handshake_cb = device_api_console_ws_opened,
    };

    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &console_ws), TAG, "registering %s failed", console_ws.uri);
    return ESP_OK;
}
