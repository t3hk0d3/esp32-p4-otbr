// WebSocket /zigbee: the HTTP server task's handlers, and the sender task that has the server task send frames to the
// client. One binary frame per protocol message. The server task only posts to the zigbee task's mailbox; the zigbee
// task never touches a socket: frames to the client go through the outbound queue to the sender (STYLEGUIDE, tasks
// and locks).
//
// Sessions: every handshake gets a slot with a session id (sid). Slots and sids live on the server task only; a frame
// or a close is applied to a socket only when the slot with that fd still has that sid, so a reused fd never gets the
// frames of the socket it replaced.
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "board_cores.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "zigbee_priv.h"

static const char *TAG = "zigbee";

#define SESSIONS 8 // more than the server's open sockets: a closing one never crowds out a new one
#define SENDER_STACK 4096
#define SENDER_PRIORITY 5
#define LRU_REFRESH_MS 5000          // a quiet coordinator link is not the socket the server purges
#define RETRY_MS 20                  // a socket with no room in its send buffer is tried again after this
#define STALLED_US (15 * 1000 * 1000) // a client that has not read for this long is closed

typedef struct {
    bool active;
    int fd;
    uint32_t sid;
} session_t;

static session_t s_sessions[SESSIONS]; // server task only
static uint32_t s_next_sid = 1;        // server task only; 0 is "no session"
static TaskHandle_t s_sender;

/* ---- server task ---- */

static session_t *session_of(int fd, uint32_t sid)
{
    for (size_t i = 0; i < SESSIONS; i++) {
        if (s_sessions[i].active && s_sessions[i].fd == fd && s_sessions[i].sid == sid) {
            return &s_sessions[i];
        }
    }
    return NULL;
}

static bool is_session(const void *ctx)
{
    const session_t *s = ctx;

    return s >= s_sessions && s < s_sessions + SESSIONS && s->active;
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

// The socket closed (either side): free_ctx of its session.
static void session_free(void *ctx)
{
    session_t *s = ctx;
    const zigbee_msg_t m = {.kind = ZIGBEE_MSG_DISCONNECT, .fd = s->fd, .sid = s->sid};

    if (!zigbee_post_msg(&m, true)) {
        ESP_LOGW(TAG, "disconnect of socket %d lost (mailbox full)", s->fd);
    }
    s->active = false;
}

// Before the upgrade: no stack, no client (the skeleton's "no channel running ziggurat: refused").
static esp_err_t ws_handshake(httpd_req_t *req)
{
    if (!atomic_load(&g_zigbee.started)) {
        ESP_LOGW(TAG, "WebSocket refused: no channel runs ziggurat");
        return ESP_FAIL; // the server closes the connection without upgrading it
    }
    return ESP_OK;
}

static esp_err_t ws_opened(httpd_req_t *req)
{
    const int fd = httpd_req_to_sockfd(req);
    session_t *s = NULL;

    for (size_t i = 0; i < SESSIONS && !s; i++) {
        if (!s_sessions[i].active) {
            s = &s_sessions[i];
        }
    }
    ESP_RETURN_ON_FALSE(s, ESP_FAIL, TAG, "too many /zigbee sockets");
    char addr[ZIGBEE_ADDRESS_MAX];

    peer_address(fd, addr, sizeof(addr));
    zigbee_frame_t *text = zigbee_frame_copy((const uint8_t *)addr, strlen(addr) + 1);

    ESP_RETURN_ON_FALSE(text, ESP_ERR_NO_MEM, TAG, "no memory for a client");
    s->active = true;
    s->fd = fd;
    s->sid = s_next_sid++;
    if (s_next_sid == 0) {
        s_next_sid = 1;
    }
    req->sess_ctx = s;
    req->free_ctx = session_free;
    const zigbee_msg_t m = {.kind = ZIGBEE_MSG_CONNECT, .fd = fd, .sid = s->sid, .frame = text};

    if (!zigbee_post_msg(&m, true)) {
        zigbee_frame_free(text);
        ESP_LOGW(TAG, "client %s refused: the zigbee task is behind", addr);
        return ESP_FAIL; // closed; its free_ctx posts a disconnect the zigbee task ignores
    }
    return ESP_OK;
}

static esp_err_t ws_frame(httpd_req_t *req)
{
    session_t *s = req->sess_ctx;

    if (!is_session(s)) { // a plain GET, or another handler's session
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "WebSocket only");
        return ESP_FAIL;
    }
    httpd_ws_frame_t ws = {0};

    ESP_RETURN_ON_ERROR(httpd_ws_recv_frame(req, &ws, 0), TAG, "reading a frame header");
    if (zigbee_link_must_close(ws.len)) {
        ESP_LOGW(TAG, "frame of %u bytes: closing the socket", (unsigned)ws.len);
        return ESP_FAIL; // its payload is still unread: drop the socket
    }
    if (ws.len == 0) {
        return ESP_OK;
    }
    zigbee_frame_t *f = zigbee_frame_alloc(ws.len);

    ESP_RETURN_ON_FALSE(f, ESP_ERR_NO_MEM, TAG, "no memory for a frame");
    ws.payload = f->data;
    esp_err_t err = httpd_ws_recv_frame(req, &ws, ws.len);

    if (err != ESP_OK || ws.type != HTTPD_WS_TYPE_BINARY || !ws.final) {
        zigbee_frame_free(f); // text and fragmented frames are ignored
        return err;
    }
    httpd_sess_update_lru_counter(req->handle, s->fd);
    const zigbee_msg_t m = {.kind = ZIGBEE_MSG_FRAME, .fd = s->fd, .sid = s->sid, .frame = f};

    if (!zigbee_post_msg(&m, false)) {
        zigbee_frame_free(f);
        ESP_LOGW(TAG, "the zigbee task is behind: closing the client");
        return ESP_FAIL; // a lost frame would desynchronise the protocol; the client reconnects
    }
    return ESP_OK;
}

esp_err_t zigbee_register_ws(httpd_handle_t server)
{
    ESP_RETURN_ON_FALSE(server != NULL, ESP_ERR_INVALID_ARG, TAG, "no server");
    atomic_store(&g_zigbee.server, server);
    const httpd_uri_t uri = {
        .uri = "/zigbee",
        .method = HTTP_GET,
        .handler = ws_frame,
        .is_websocket = true,
        .ws_pre_handshake_cb = ws_handshake,
        .ws_post_handshake_cb = ws_opened,
    };

    return httpd_register_uri_handler(server, &uri);
}

/* ---- sender: work it runs on the server task ---- */

typedef enum {
    JOB_SEND,
    JOB_CLOSE,
    JOB_LRU,
} job_kind_t;

typedef struct {
    job_kind_t kind;
    httpd_handle_t hd;
    int fd;
    uint32_t sid;
    const zigbee_frame_t *frame;
    esp_err_t result; // JOB_SEND: ESP_OK sent, ESP_ERR_TIMEOUT no room yet, ESP_ERR_NOT_FOUND gone, else failed
} job_t;

static job_t s_job; // the sender's; one job at a time

static bool writable(int fd)
{
    fd_set w;
    struct timeval none = {0};

    FD_ZERO(&w);
    FD_SET(fd, &w);
    return select(fd + 1, NULL, &w, NULL, &none) > 0;
}

static void job_run(void *arg)
{
    job_t *j = arg;

    j->result = ESP_ERR_NOT_FOUND;
    if (session_of(j->fd, j->sid) != NULL && httpd_ws_get_fd_info(j->hd, j->fd) == HTTPD_WS_CLIENT_WEBSOCKET) {
        switch (j->kind) {
        case JOB_SEND: {
            if (!writable(j->fd)) { // never block the server task on a client that does not read
                j->result = ESP_ERR_TIMEOUT;
                break;
            }
            httpd_ws_frame_t ws = {
                .type = HTTPD_WS_TYPE_BINARY,
                .final = true,
                .payload = (uint8_t *)j->frame->data,
                .len = j->frame->len,
            };

            j->result = httpd_ws_send_frame_async(j->hd, j->fd, &ws);
            if (j->result == ESP_OK) {
                httpd_sess_update_lru_counter(j->hd, j->fd);
            } else {
                ESP_LOGW(TAG, "sending to socket %d failed: closing it", j->fd);
                httpd_sess_trigger_close(j->hd, j->fd);
            }
            break;
        }
        case JOB_CLOSE:
            httpd_sess_trigger_close(j->hd, j->fd);
            j->result = ESP_OK;
            break;
        case JOB_LRU:
            httpd_sess_update_lru_counter(j->hd, j->fd);
            j->result = ESP_OK;
            break;
        }
    }
    xSemaphoreGive(g_zigbee.job_done);
}

// Runs one job on the server task and waits for it. The server task never waits for the sender.
static esp_err_t job(job_kind_t kind, int fd, uint32_t sid, const zigbee_frame_t *frame)
{
    const httpd_handle_t hd = atomic_load(&g_zigbee.server);

    if (hd == NULL || fd < 0) {
        return ESP_ERR_INVALID_STATE;
    }
    s_job = (job_t) {
        .kind = kind, .hd = hd, .fd = fd, .sid = sid, .frame = frame,
    };
    const esp_err_t err = httpd_queue_work(hd, job_run, &s_job);

    if (err != ESP_OK) {
        return err;
    }
    xSemaphoreTake(g_zigbee.job_done, portMAX_DELAY);
    return s_job.result;
}

// One frame: sent, or dropped once its client is gone or replaced (a newer generation), or the client is closed when
// it has not read for STALLED_US (the queue behind it is full by then, so the stack waits for the ready signal).
static void deliver(const zigbee_out_t *it)
{
    const int64_t since = esp_timer_get_time();

    for (;;) {
        if (it->gen != atomic_load(&g_zigbee.gen)) {
            return; // stale: its client left or was replaced
        }
        const esp_err_t err = job(JOB_SEND, it->fd, it->sid, it->frame);

        if (err == ESP_OK) {
            ZIGBEE_STATUS(g_zigbee_status.counters.frames_out++);
            return;
        }
        if (err != ESP_ERR_TIMEOUT) {
            return; // gone, or failed and closed
        }
        if (esp_timer_get_time() - since > STALLED_US) {
            ESP_LOGW(TAG, "client on socket %d does not read: closing it", it->fd);
            job(JOB_CLOSE, it->fd, it->sid, NULL);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(RETRY_MS));
    }
}

static void sender_task(void *arg)
{
    zigbee_t *z = &g_zigbee;

    for (;;) {
        zigbee_out_t it;

        if (xQueueReceive(z->out, &it, pdMS_TO_TICKS(LRU_REFRESH_MS)) != pdTRUE) {
            job(JOB_LRU, atomic_load(&z->client_fd), atomic_load(&z->client_sid), NULL);
            continue;
        }
        if (it.kind == ZIGBEE_OUT_CLOSE) {
            job(JOB_CLOSE, it.fd, it.sid, NULL);
        } else {
            deliver(&it);
        }
        zigbee_frame_free(it.frame);
        if (it.counted) { // sent or discarded: its slot is free (zigbee_link_sent on the zigbee task)
            atomic_fetch_add(&z->sent, 1);
            zigbee_kick();
        }
    }
}

esp_err_t zigbee_ws_start_sender(void)
{
    if (s_sender == NULL &&
            xTaskCreatePinnedToCore(sender_task, "zigbee_tx", SENDER_STACK, NULL, SENDER_PRIORITY, &s_sender, MGMT_CORE)
            != pdPASS) {
        s_sender = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
