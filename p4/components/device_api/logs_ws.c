// GET /device/logging/ws: the live log (spec 2026-11-10-logging, "Interfaces"). Each client is a logging viewer with
// its own level (?level= at connect, then text frames {"level":"<name>"}); it gets one text frame per line from the
// moment it connected, "<L> (<uptime>) <tag>: <body>", and "# dropped <n> lines" when its queue overflowed. Up to
// LOGGING_VIEWERS_MAX clients (the handshake refuses a fifth with 503). As /device/console/ws (console_ws.c): the board
// pings every 10 s and closes a client that has not answered for 30 s (1001); ESP-IDF answers the client's pings and
// close. Unlike the console, a slow client is never closed for being slow: its queue drops lines and it is told so.
//
// Tasks: the drain task ("log_out") queues lines and calls wake, which only notifies the forwarder here; the
// forwarder queues the sends on the server task (httpd_queue_work), and the esp_timer tick goes the same way, so
// neither the drain task nor the esp_timer task ever reaches lwIP. The sends run inside logging_sink_enter/leave: what
// lwIP or the server logs meanwhile goes to the UART only and is never sent again. A socket whose send buffer is full is
// skipped (no blocking send on the server task) until the next tick; its queue drops meanwhile.
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "logging.h"
#include "logging_json.h"
#include "logs_ws.h"

static const char *TAG = "device_api";

// More than the server's open sockets (ESP_BR_WEB_MAX_OPEN_SOCKETS, 7): a closing socket (up to CLOSE_WAIT_US) never
// crowds out a new one. The viewers, not the slots, are what LOGGING_VIEWERS_MAX bounds.
#define SLOTS 8
_Static_assert(2 * SLOTS <= 32, "drain and tick bits share one notification word");
#define TICK_US (1000 * 1000) // also how often a client with a full send buffer is retried
#define PING_US (10 * 1000 * 1000)
#define NO_ANSWER_US (30 * 1000 * 1000)
#define CLOSE_WAIT_US (2 * 1000 * 1000) // for the client's close frame, after which ESP-IDF closes the socket
#define FRAME_IN_MAX 128 // {"level":"verbose"} with room to spare
#define BATCH_LINES 32 // lines sent per work item before the server task serves others; the rest follow at once
#define CLOSE_GOING_AWAY 1001
#define CLOSE_TOO_BIG 1009
#define CLOSE_TRY_AGAIN 1013
#define FWD_STACK 3072
#define FWD_PRIORITY 5
#define DRAIN_BIT(i) (1u << (i))
#define TICK_BIT(i) (1u << (SLOTS + (i)))
#define LEVEL_HELP "# ignored: send {\"level\":\"off|error|warning|info|debug|verbose\"}"

// One socket. Slots and their timers are static and never freed: a wake or a tick queued before the socket closed
// finds the slot inactive (or reused, then it drains the new session, which is harmless) instead of freed memory.
typedef struct {
    bool active;
    httpd_handle_t hd;
    int fd;
    logging_viewer_t *viewer; // NULL once closing: the viewer is closed with the close frame
    esp_timer_handle_t timer;
    int64_t last_pong_us;
    int64_t last_ping_us;
    int64_t closing_us;      // our close frame went out (0: open)
    atomic_bool drain_queued;
    atomic_bool stalled;     // the send buffer was full: wakes are ignored until the tick retries
} ws_slot_t;

static ws_slot_t s_slots[SLOTS];
static char s_out[DEVICE_API_LOGS_FRAME_MAX]; // used on the server task only
// Queues drains and ticks on the server task, so the drain task and the esp_timer task never reach lwIP.
static TaskHandle_t s_fwd;

static esp_err_t send_text(ws_slot_t *slot, const char *text, size_t len)
{
    httpd_ws_frame_t frame = {.type = HTTPD_WS_TYPE_TEXT, .final = true, .payload = (uint8_t *)text, .len = len};

    return httpd_ws_send_frame_async(slot->hd, slot->fd, &frame);
}

// A notice to one client, as a sink (see the top of the file). On the server task.
static void send_notice(ws_slot_t *slot, const char *text)
{
    logging_sink_enter();
    send_text(slot, text, strlen(text));
    logging_sink_leave();
}

static void viewer_close(ws_slot_t *slot)
{
    if (slot->viewer) {
        logging_viewer_close(slot->viewer); // no wake for it after this
        slot->viewer = NULL;
    }
}

// Close frame with a code and reason; the viewer stops at once. On the server task. A slot's socket stays open for the
// client's close frame; the tick closes it after CLOSE_WAIT_US. A socket without a slot is closed at once.
static void close_with(ws_slot_t *slot, uint16_t code, const char *reason)
{
    uint8_t payload[2 + 64];
    const size_t reason_len = reason ? strnlen(reason, sizeof(payload) - 2) : 0;

    viewer_close(slot);
    payload[0] = (uint8_t)(code >> 8);
    payload[1] = (uint8_t)code;
    memcpy(payload + 2, reason, reason_len);
    httpd_ws_frame_t frame = {.type = HTTPD_WS_TYPE_CLOSE, .final = true, .payload = payload, .len = 2 + reason_len};

    logging_sink_enter();
    httpd_ws_send_frame_async(slot->hd, slot->fd, &frame);
    logging_sink_leave();
    if (slot->active && slot->timer) {
        slot->closing_us = esp_timer_get_time();
    } else {
        httpd_sess_trigger_close(slot->hd, slot->fd);
    }
}

// Whether the socket's send buffer has room for a frame now: lwIP reports a TCP socket writable while at least
// TCP_SNDLOWAT bytes are free, several of the longest frames.
static bool writable(int fd)
{
    fd_set set;
    struct timeval now = {0};

    FD_ZERO(&set);
    FD_SET(fd, &set);
    return select(fd + 1, NULL, &set, NULL, &now) > 0;
}

static void request_drain(ws_slot_t *slot)
{
    if (!atomic_exchange(&slot->drain_queued, true)) {
        xTaskNotify(s_fwd, DRAIN_BIT(slot - s_slots), eSetBits);
    }
}

// Send what the viewer holds, a batch at a time, as a sink. On the server task.
static void drain(ws_slot_t *slot)
{
    logging_line_t line;
    uint32_t dropped;
    bool more = false;
    int n;

    if (!slot->active || slot->closing_us || !slot->viewer) {
        return;
    }
    logging_sink_enter();
    for (int i = 0; i < BATCH_LINES; i++) {
        if (!writable(slot->fd)) {
            atomic_store(&slot->stalled, true); // the tick retries; the queue drops meanwhile
            break;
        }
        atomic_store(&slot->stalled, false);
        const bool got = logging_viewer_read(slot->viewer, &line, &dropped);

        if (dropped && (n = device_api_logs_dropped(s_out, sizeof(s_out), dropped)) > 0 &&
                send_text(slot, s_out, (size_t)n) != ESP_OK) {
            httpd_sess_trigger_close(slot->hd, slot->fd);
            break;
        }
        if (!got) {
            break;
        }
        if ((n = device_api_logs_line(s_out, sizeof(s_out), &line)) > 0 &&
                send_text(slot, s_out, (size_t)n) != ESP_OK) {
            httpd_sess_trigger_close(slot->hd, slot->fd);
            break;
        }
        more = i == BATCH_LINES - 1;
    }
    logging_sink_leave();
    if (more) {
        request_drain(slot); // the rest after whatever else the server task has queued
    }
}

static void drain_work(void *arg)
{
    ws_slot_t *slot = arg;

    atomic_store(&slot->drain_queued, false);
    drain(slot);
}

// The viewer's wake: on the drain task with the viewers' lock held, so it only notifies the forwarder.
static void wake(void *ctx)
{
    ws_slot_t *slot = ctx;

    if (!atomic_load(&slot->stalled)) {
        request_drain(slot);
    }
}

static void tick_work(void *arg)
{
    ws_slot_t *slot = arg;

    if (!slot->active) {
        return;
    }
    const int64_t now = esp_timer_get_time();

    if (slot->closing_us) {
        if (now - slot->closing_us > CLOSE_WAIT_US) {
            httpd_sess_trigger_close(slot->hd, slot->fd);
        }
        return;
    }
    if (now - slot->last_pong_us > NO_ANSWER_US) {
        ESP_LOGW(TAG, "live log: no answer to pings for 30 s, closing");
        close_with(slot, CLOSE_GOING_AWAY, "no answer");
        return;
    }
    httpd_sess_update_lru_counter(slot->hd, slot->fd); // a viewer that answers is not the socket to purge
    drain(slot); // also retries a stalled socket and a drain the forwarder could not queue
    if (now - slot->last_ping_us >= PING_US) {
        httpd_ws_frame_t ping = {.type = HTTPD_WS_TYPE_PING, .final = true};

        slot->last_ping_us = now;
        logging_sink_enter();
        httpd_ws_send_frame_async(slot->hd, slot->fd, &ping);
        logging_sink_leave();
    }
}

// On the esp_timer task: only notifies the forwarder (see s_fwd).
static void tick(void *arg)
{
    ws_slot_t *slot = arg;

    xTaskNotify(s_fwd, TICK_BIT(slot - s_slots), eSetBits);
}

static void forwarder(void *arg)
{
    uint32_t bits;

    // For good: a failing httpd_queue_work must not log a line that wakes this task again.
    logging_sink_enter();
    for (;;) {
        xTaskNotifyWait(0, UINT32_MAX, &bits, portMAX_DELAY);
        for (size_t i = 0; i < SLOTS; i++) {
            ws_slot_t *slot = &s_slots[i];

            if ((bits & DRAIN_BIT(i)) && httpd_queue_work(slot->hd, drain_work, slot) != ESP_OK) {
                atomic_store(&slot->drain_queued, false); // the tick retries
            }
            if (bits & TICK_BIT(i)) {
                httpd_queue_work(slot->hd, tick_work, slot);
            }
        }
    }
}

// The forwarder and the slot's timer, made once. On the server task.
static esp_err_t slot_prepare(ws_slot_t *slot)
{
    if (!s_fwd && xTaskCreatePinnedToCore(forwarder, "logs_ws", FWD_STACK, NULL, FWD_PRIORITY, &s_fwd,
                                          tskNO_AFFINITY) != pdPASS) {
        s_fwd = NULL;
        return ESP_ERR_NO_MEM;
    }
    if (!slot->timer) {
        const esp_timer_create_args_t args = {.callback = tick, .arg = slot, .name = "logs_ws"};

        return esp_timer_create(&args, &slot->timer);
    }
    return ESP_OK;
}

// The socket closed (either side, or the server stopped): free_ctx of its session.
static void slot_free(void *ctx)
{
    ws_slot_t *slot = ctx;

    esp_timer_stop(slot->timer);
    viewer_close(slot);
    slot->active = false;
}

size_t device_api_logs_ws_viewers(void)
{
    size_t n = 0;

    for (size_t i = 0; i < SLOTS; i++) {
        n += s_slots[i].active && s_slots[i].viewer ? 1 : 0;
    }
    return n;
}

// The level of the handshake's URL; the pre-handshake check refused a bad one, so this only falls back to info.
static esp_log_level_t request_level(httpd_req_t *req)
{
    esp_log_level_t level = ESP_LOG_INFO;
    const size_t len = httpd_req_get_url_query_len(req);
    char *query = len ? malloc(len + 1) : NULL;

    if (query && httpd_req_get_url_query_str(req, query, len + 1) == ESP_OK &&
            device_api_logs_query_level(query, &level) != ESP_OK) {
        level = ESP_LOG_INFO;
    }
    free(query);
    return level;
}

esp_err_t device_api_logs_ws_opened(httpd_req_t *req)
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
        close_with(&refused, CLOSE_TRY_AGAIN, "too many live log sockets");
        return ESP_OK;
    }
    if (slot_prepare(slot) != ESP_OK) {
        close_with(&refused, CLOSE_TRY_AGAIN, "no memory");
        return ESP_OK;
    }
    const int64_t now = esp_timer_get_time();

    slot->hd = req->handle;
    slot->fd = fd;
    slot->viewer = NULL;
    slot->last_pong_us = slot->last_ping_us = now;
    slot->closing_us = 0;
    atomic_store(&slot->drain_queued, false);
    atomic_store(&slot->stalled, false);
    slot->active = true;
    if (esp_timer_start_periodic(slot->timer, TICK_US) != ESP_OK) {
        slot->active = false;
        close_with(&refused, CLOSE_TRY_AGAIN, "no timer");
        return ESP_OK;
    }
    req->sess_ctx = slot;
    req->free_ctx = slot_free;
    slot->viewer = logging_viewer_open(request_level(req), wake, slot);
    if (!slot->viewer) { // all taken in between (the handshake checked), or no PSRAM for the queue
        close_with(slot, CLOSE_TRY_AGAIN, DEVICE_API_LOGS_TOO_MANY);
    }
    return ESP_OK;
}

esp_err_t device_api_logs_ws_frame(httpd_req_t *req)
{
    ws_slot_t *slot = req->sess_ctx;
    httpd_ws_frame_t frame = {0};
    char buf[FRAME_IN_MAX + 1];

    ESP_RETURN_ON_ERROR(httpd_ws_recv_frame(req, &frame, 0), TAG, "live log: reading a frame header");
    if (!slot || !slot->active) {
        return ESP_FAIL;
    }
    if (frame.len > FRAME_IN_MAX) {
        close_with(slot, CLOSE_TOO_BIG, "frame too long");
        return ESP_FAIL; // its payload is still unread: drop the socket now
    }
    frame.payload = (uint8_t *)buf;
    esp_err_t err = frame.len ? httpd_ws_recv_frame(req, &frame, frame.len) : ESP_OK;

    if (err != ESP_OK) {
        return err;
    }
    if (frame.type == HTTPD_WS_TYPE_PONG) {
        slot->last_pong_us = esp_timer_get_time();
        httpd_sess_update_lru_counter(slot->hd, slot->fd); // an idle viewer is not the socket to purge
    } else if (frame.type == HTTPD_WS_TYPE_TEXT && slot->viewer && !slot->closing_us) {
        esp_log_level_t level;

        if (device_api_logs_level_frame(buf, frame.len, &level) == ESP_OK) {
            logging_viewer_set_level(slot->viewer, level); // this client only; the gate follows
        } else {
            send_notice(slot, LEVEL_HELP);
        }
    }
    return ESP_OK;
}
