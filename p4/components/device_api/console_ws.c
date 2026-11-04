// GET /device/console/ws: the Thread console's WebSocket transport (spec 2026-11-04-thread-console). One text frame per
// line in; CLI output and "# " notices out as text frames. The board pings every 10 s and closes a client that has not
// answered for 30 s (1001) or has not drained its output for 15 s (4009). ESP-IDF answers the client's pings and close.
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "console_ws.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "ot_console.h"
#include "ot_console_core.h"

static const char *TAG = "device_api";

#define SLOTS 4 // an open session, one being taken over, and room for a refusal
#define TICK_US (5 * 1000 * 1000)
#define PING_US (10 * 1000 * 1000)
#define NO_ANSWER_US (30 * 1000 * 1000)
#define STALLED_US (15 * 1000 * 1000)
#define FRAME_IN_MAX 1024 // longer lines are refused by the console anyway; this only bounds the read
#define FRAME_OUT_MAX 1024
#define CLOSE_GOING_AWAY 1001
#define CLOSE_TOO_BIG 1009
#define CLOSE_TRY_AGAIN 1013

// One socket. Slots are static: a wake or a tick queued before the socket closed finds the slot inactive (or reused,
// then it drains the new session, which is harmless) instead of freed memory.
typedef struct {
    bool active;
    httpd_handle_t hd;
    int fd;
    uint32_t id;
    esp_timer_handle_t timer;
    int64_t last_pong_us;
    int64_t last_ping_us;
    int64_t last_progress_us; // output was drained, or nothing waited
    atomic_bool drain_queued;
} ws_slot_t;

static ws_slot_t s_slots[SLOTS];
static char s_out[FRAME_OUT_MAX]; // used on the server task only

static esp_err_t send_text(ws_slot_t *slot, const char *text, size_t len)
{
    httpd_ws_frame_t frame = {.type = HTTPD_WS_TYPE_TEXT, .final = true, .payload = (uint8_t *)text, .len = len};

    return httpd_ws_send_frame_async(slot->hd, slot->fd, &frame);
}

// Close frame with a code and reason, then close the socket. On the server task.
static void close_with(ws_slot_t *slot, uint16_t code, const char *reason)
{
    uint8_t payload[2 + 64];
    const size_t reason_len = reason ? strnlen(reason, sizeof(payload) - 2) : 0;

    payload[0] = (uint8_t)(code >> 8);
    payload[1] = (uint8_t)code;
    memcpy(payload + 2, reason, reason_len);
    httpd_ws_frame_t frame = {.type = HTTPD_WS_TYPE_CLOSE, .final = true, .payload = payload, .len = 2 + reason_len};

    httpd_ws_send_frame_async(slot->hd, slot->fd, &frame);
    httpd_sess_trigger_close(slot->hd, slot->fd);
}

static void drain_work(void *arg)
{
    ws_slot_t *slot = arg;

    atomic_store(&slot->drain_queued, false);
    if (!slot->active) {
        return;
    }
    size_t n;

    while ((n = ot_console_take(slot->id, s_out, sizeof(s_out))) > 0) {
        if (send_text(slot, s_out, n) != ESP_OK) {
            httpd_sess_trigger_close(slot->hd, slot->fd);
            return;
        }
        slot->last_progress_us = esp_timer_get_time();
    }
}

// The console's wake: from the OpenThread task too, so it only queues the drain on the server task.
static void wake(void *ctx)
{
    ws_slot_t *slot = ctx;

    if (!atomic_exchange(&slot->drain_queued, true)) {
        if (httpd_queue_work(slot->hd, drain_work, slot) != ESP_OK) {
            atomic_store(&slot->drain_queued, false);
        }
    }
}

// The console ended the session (take-over, turned off). On the server task (attach and the PUT handler run there).
static void sink_close(void *ctx, uint16_t code, const char *notice)
{
    ws_slot_t *slot = ctx;

    if (!slot->active) {
        return;
    }
    send_text(slot, notice, strlen(notice));
    close_with(slot, code, NULL);
}

static void tick_work(void *arg)
{
    ws_slot_t *slot = arg;

    if (!slot->active) {
        return;
    }
    const int64_t now = esp_timer_get_time();

    if (now - slot->last_pong_us > NO_ANSWER_US) {
        ESP_LOGW(TAG, "console: no answer to pings for 30 s, closing");
        close_with(slot, CLOSE_GOING_AWAY, "no answer");
        return;
    }
    if (ot_console_queued(slot->id) == 0) {
        slot->last_progress_us = now;
    } else if (now - slot->last_progress_us > STALLED_US) {
        ESP_LOGW(TAG, "console: output not drained for 15 s, closing");
        close_with(slot, OT_CONSOLE_CLOSE_STALLED, "output not drained");
        return;
    }
    if (now - slot->last_ping_us >= PING_US) {
        httpd_ws_frame_t ping = {.type = HTTPD_WS_TYPE_PING, .final = true};

        slot->last_ping_us = now;
        httpd_ws_send_frame_async(slot->hd, slot->fd, &ping);
    }
}

static void tick(void *arg)
{
    ws_slot_t *slot = arg;

    httpd_queue_work(slot->hd, tick_work, slot);
}

// The socket closed (either side): free_ctx of its session.
static void slot_free(void *ctx)
{
    ws_slot_t *slot = ctx;

    if (slot->timer) {
        esp_timer_stop(slot->timer);
        esp_timer_delete(slot->timer);
        slot->timer = NULL;
    }
    ot_console_detach(slot->id);
    slot->active = false;
}

static void peer_address(int fd, char *out, size_t cap)
{
    struct sockaddr_storage addr;
    socklen_t len = sizeof(addr);

    strlcpy(out, "?", cap);
    if (getpeername(fd, (struct sockaddr *)&addr, &len) != 0) {
        return;
    }
    if (addr.ss_family == AF_INET) {
        inet_ntop(AF_INET, &((struct sockaddr_in *)&addr)->sin_addr, out, cap);
    } else if (addr.ss_family == AF_INET6) {
        const struct sockaddr_in6 *a6 = (const struct sockaddr_in6 *)&addr;

        if (IN6_IS_ADDR_V4MAPPED(&a6->sin6_addr)) { // an IPv4 client on the dual-stack socket
            inet_ntop(AF_INET, &a6->sin6_addr.s6_addr[12], out, cap);
        } else {
            inet_ntop(AF_INET6, &a6->sin6_addr, out, cap);
        }
    }
}

esp_err_t device_api_console_ws_opened(httpd_req_t *req)
{
    const int fd = httpd_req_to_sockfd(req);
    ws_slot_t *slot = NULL;

    for (size_t i = 0; i < SLOTS && !slot; i++) {
        if (!s_slots[i].active) {
            slot = &s_slots[i];
        }
    }
    ws_slot_t refused = {.hd = req->handle, .fd = fd};

    if (!slot) {
        close_with(&refused, CLOSE_TRY_AGAIN, "too many console sockets");
        return ESP_OK;
    }
    if (!ot_console_enabled()) {
        char text[64];

        ot_console_notice_turned_off(text, sizeof(text));
        send_text(&refused, text, strlen(text));
        close_with(&refused, OT_CONSOLE_CLOSE_TURNED_OFF, NULL);
        return ESP_OK;
    }
    const int64_t now = esp_timer_get_time();

    *slot = (ws_slot_t) {
        .active = true, .hd = req->handle, .fd = fd, .last_pong_us = now, .last_ping_us = now, .last_progress_us = now,
    };
    const esp_timer_create_args_t args = {.callback = tick, .arg = slot, .name = "console_ws"};

    if (esp_timer_create(&args, &slot->timer) != ESP_OK || esp_timer_start_periodic(slot->timer, TICK_US) != ESP_OK) {
        slot_free(slot);
        close_with(&refused, CLOSE_TRY_AGAIN, "no timer");
        return ESP_OK;
    }
    char peer[48];

    peer_address(fd, peer, sizeof(peer));
    const ot_console_sink_t sink = {.wake = wake, .close = sink_close, .ctx = slot};

    req->sess_ctx = slot;
    req->free_ctx = slot_free;
    if (ot_console_attach(peer, sink, &slot->id) != ESP_OK) { // turned off in between
        close_with(slot, OT_CONSOLE_CLOSE_TURNED_OFF, NULL);
    }
    return ESP_OK;
}

esp_err_t device_api_console_ws_frame(httpd_req_t *req)
{
    ws_slot_t *slot = req->sess_ctx;
    httpd_ws_frame_t frame = {0};

    ESP_RETURN_ON_ERROR(httpd_ws_recv_frame(req, &frame, 0), TAG, "console: reading a frame header");
    if (!slot || !slot->active) {
        return ESP_FAIL;
    }
    if (frame.len > FRAME_IN_MAX) {
        close_with(slot, CLOSE_TOO_BIG, "line too long");
        return ESP_OK;
    }
    char *buf = calloc(1, frame.len + 1);

    ESP_RETURN_ON_FALSE(buf, ESP_ERR_NO_MEM, TAG, "console: no memory for a frame");
    frame.payload = (uint8_t *)buf;
    esp_err_t err = frame.len ? httpd_ws_recv_frame(req, &frame, frame.len) : ESP_OK;

    if (err == ESP_OK) {
        if (frame.type == HTTPD_WS_TYPE_PONG) {
            slot->last_pong_us = esp_timer_get_time();
            httpd_sess_update_lru_counter(slot->hd, slot->fd); // an idle console is not the socket to purge
        } else if (frame.type == HTTPD_WS_TYPE_TEXT) {
            err = ot_console_submit(slot->id, buf);
            if (err == ESP_ERR_INVALID_STATE) { // taken over or turned off; its close is on the way
                err = ESP_OK;
            }
        }
    }
    free(buf);
    return err;
}
