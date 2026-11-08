// The zigbee component's consumer registration, attach and detach, and the zigbee task: one loop that owns ziggurat,
// the spinel client and the WebSocket link rules (zigbee_priv.h has the task and queue layout).
#include <stdlib.h>
#include <string.h>

#include "board_cores.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "ziggurat.h"
#include "zigbee_priv.h"

static const char *TAG = "zigbee";

#define ZIGBEE_TASK_STACK 12288 // the spike ran ziggurat on the 12 KB OpenThread task
#define TICK_US (250 * 1000)    // the spinel client's transmit, re-arm and scan deadlines
#define MAILBOX_BATCH 16 // messages per mailbox round, so the mux's frames are not starved by a busy client

_Static_assert(ZIGBEE_TASK_PRIORITY == CONFIG_OPENTHREAD_TASK_PRIORITY,
               "the zigbee task runs at the OpenThread task's priority (plan, Threads)");
_Static_assert(ZIGBEE_MAILBOX_RESERVE < ZIGBEE_MAILBOX_DEPTH, "frames need room in the mailbox");

zigbee_t g_zigbee = {
    .mb_lock = portMUX_INITIALIZER_UNLOCKED,
    .in_lock = portMUX_INITIALIZER_UNLOCKED,
    .client_fd = -1,
    .timer_deadline = UINT64_MAX,
};

static bool s_registered;

/* ---- frames ---- */

zigbee_frame_t *zigbee_frame_alloc(size_t len)
{
    zigbee_frame_t *f = heap_caps_malloc(sizeof(*f) + len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (f == NULL) {
        f = malloc(sizeof(*f) + len);
    }
    if (f != NULL) {
        f->len = len;
    }
    return f;
}

zigbee_frame_t *zigbee_frame_copy(const uint8_t *data, size_t len)
{
    zigbee_frame_t *f = zigbee_frame_alloc(len);

    if (f != NULL && len > 0) {
        memcpy(f->data, data, len);
    }
    return f;
}

void zigbee_frame_free(zigbee_frame_t *f)
{
    free(f);
}

/* ---- mailbox (any task) ---- */

void zigbee_kick(void)
{
    zigbee_t *z = &g_zigbee;

    // At most one KICK is in the queue (zigbee_kick.h): it shares the queue with the mux's frames, RESET and GONE,
    // which the mux posts without waiting.
    if (z->q == NULL || !zigbee_kick_claim(&z->kick)) {
        return;
    }
    const radio_mux_ev_t ev = {.type = ZIGBEE_EV_KICK};

    // A full queue: the zigbee task looks at the mailbox after every item it takes, so nothing is stranded.
    if (xQueueSend(z->q, &ev, 0) != pdTRUE) {
        zigbee_kick_unclaim(&z->kick);
    }
}

bool zigbee_post_msg(const zigbee_msg_t *m, bool control)
{
    zigbee_t *z = &g_zigbee;
    const size_t cap = control ? ZIGBEE_MAILBOX_DEPTH : ZIGBEE_MAILBOX_DEPTH - ZIGBEE_MAILBOX_RESERVE;
    bool ok;

    taskENTER_CRITICAL(&z->mb_lock);
    ok = z->mb_count < cap;
    if (ok) {
        z->mb[(z->mb_head + z->mb_count) % ZIGBEE_MAILBOX_DEPTH] = *m;
        z->mb_count++;
    }
    taskEXIT_CRITICAL(&z->mb_lock);
    if (ok) {
        zigbee_kick();
    }
    return ok;
}

void zigbee_post_flags(uint32_t flags)
{
    zigbee_t *z = &g_zigbee;

    taskENTER_CRITICAL(&z->mb_lock);
    z->mb_flags |= flags;
    taskEXIT_CRITICAL(&z->mb_lock);
    zigbee_kick();
}

static uint32_t take_flags(void)
{
    zigbee_t *z = &g_zigbee;

    taskENTER_CRITICAL(&z->mb_lock);
    const uint32_t flags = z->mb_flags;

    z->mb_flags = 0;
    taskEXIT_CRITICAL(&z->mb_lock);
    return flags;
}

// Takes only ZIGBEE_FLAG_WAKE (ziggurat's pender), leaving the other flags for the next round.
static bool take_wake(void)
{
    zigbee_t *z = &g_zigbee;

    taskENTER_CRITICAL(&z->mb_lock);
    const bool wake = (z->mb_flags & ZIGBEE_FLAG_WAKE) != 0;

    z->mb_flags &= ~ZIGBEE_FLAG_WAKE;
    taskEXIT_CRITICAL(&z->mb_lock);
    return wake;
}

static bool take_msg(zigbee_msg_t *m)
{
    zigbee_t *z = &g_zigbee;
    bool ok;

    taskENTER_CRITICAL(&z->mb_lock);
    ok = z->mb_count > 0;
    if (ok) {
        *m = z->mb[z->mb_head];
        z->mb_head = (z->mb_head + 1) % ZIGBEE_MAILBOX_DEPTH;
        z->mb_count--;
    }
    taskEXIT_CRITICAL(&z->mb_lock);
    return ok;
}

/* ---- outbound (zigbee task) ---- */

bool zigbee_out_post(const zigbee_out_t *item)
{
    if (xQueueSend(g_zigbee.out, item, 0) != pdTRUE) {
        zigbee_frame_free(item->frame);
        return false;
    }
    return true;
}

// The sender reads these to drop stale frames and to keep the client's socket fresh. Published before any frame of a
// new generation is queued, so the sender never sees a frame newer than the generation it reads.
static void publish_link(void)
{
    zigbee_t *z = &g_zigbee;

    atomic_store(&z->gen, zigbee_link_generation(&z->link));
    atomic_store(&z->client_sid, z->link_sid);
    atomic_store(&z->client_fd, zigbee_link_client(&z->link));
}

/* ---- the spinel client's board side (zigbee task) ---- */

static bool handle_event(const radio_mux_ev_t *ev);
static bool take_input(void);

static int op_send(void *ctx, const uint8_t *frame, size_t len)
{
    return radio_mux_tx(atomic_load(&g_zigbee.ch), frame, len) == ESP_OK ? 0 : -1;
}

static int op_request_reset(void *ctx)
{
    const esp_err_t err = radio_mux_reset(atomic_load(&g_zigbee.ch));

    ESP_LOGI(TAG, "asking for a radio reset");
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "channel reset not possible: %s", esp_err_to_name(err));
    }
    return err == ESP_OK ? 0 : 1;
}

static int64_t op_now_us(void *ctx)
{
    return esp_timer_get_time();
}

// A property request waits here (zigbee_spinel_ops_t.wait): the mux's events (and ATTACH) are handled as they come;
// a KICK is taken out of the queue but its mailbox is left for the loop (so inbound frames, connects and ziggurat's
// timer, wake and ready keep their order and are never handled inside a ziggurat call), and kick_swallowed makes the
// loop look at the mailbox before it blocks again. Never blocks past `until_us`, and never on anything but the queue
// the mux posts into without waiting: it cannot deadlock.
static int op_wait(void *ctx, int64_t until_us)
{
    zigbee_t *z = &g_zigbee;
    const int64_t tick_us = (int64_t)portTICK_PERIOD_MS * 1000;

    for (;;) {
        const int64_t now = esp_timer_get_time();

        if (now >= until_us) {
            return 1;
        }
        TickType_t ticks = (TickType_t)((until_us - now + tick_us - 1) / tick_us);
        radio_mux_ev_t ev;

        if (xQueueReceive(z->q, &ev, ticks > 0 ? ticks : 1) != pdTRUE) {
            continue; // the clock decides
        }
        if (handle_event(&ev) || take_input()) {
            return 0;
        }
        z->kick_swallowed = true;
    }
}

static void sink_ready(void *ctx, bool compatible, const char *reason)
{
    zigbee_t *z = &g_zigbee;

    if (!compatible) {
        ESP_LOGW(TAG, "the RCP cannot serve ziggurat: %s", reason ? reason : "?");
        zigbee_status_set_state(ZIGBEE_STATE_INCOMPATIBLE, reason);
        return;
    }
    ESP_LOGI(TAG, "radio ready");
    zigbee_status_set_state(ZIGBEE_STATE_RUNNING, NULL);
    // ready may come from the end of any client call, inside a ziggurat call too: ziggurat_start runs at the top of
    // the loop, and only for the first ready of the boot.
    if (!z->start_ran) {
        z->start_due = true;
    }
}

static void sink_rx(void *ctx, const uint8_t *psdu, size_t len, uint8_t channel, int8_t rssi, uint8_t lqi,
                    uint64_t time_us)
{
    if (!g_zigbee.start_ran) {
        return;
    }
    ZIGBEE_STATUS(g_zigbee_status.counters.rx++);
    ziggurat_radio_rx(psdu, len, channel, rssi, lqi, time_us);
}

static void sink_tx_done(void *ctx, zigbee_tx_result_t r)
{
    ziggurat_tx_status status;

    switch (r) {
    case ZIGBEE_TX_ACKED:
        status = ZIGGURAT_TX_STATUS_ACKED;
        break;
    case ZIGBEE_TX_NO_ACK:
        status = ZIGGURAT_TX_STATUS_NO_ACK;
        ZIGBEE_STATUS(g_zigbee_status.counters.tx_no_ack++);
        break;
    case ZIGBEE_TX_CCA:
        status = ZIGGURAT_TX_STATUS_CHANNEL_ACCESS_FAILURE;
        ZIGBEE_STATUS(g_zigbee_status.counters.tx_cca++);
        break;
    case ZIGBEE_TX_ABORTED:
        status = ZIGGURAT_TX_STATUS_ABORTED;
        ZIGBEE_STATUS(g_zigbee_status.counters.tx_failed++);
        break;
    default:
        status = ZIGGURAT_TX_STATUS_FAILED;
        ZIGBEE_STATUS(g_zigbee_status.counters.tx_failed++);
        break;
    }
    if (g_zigbee.start_ran) {
        ziggurat_radio_tx_done(status);
    }
}

static void sink_scan_done(void *ctx, int8_t max_rssi)
{
    if (max_rssi == ZIGBEE_SPINEL_SCAN_FAILED_RSSI) {
        ESP_LOGW(TAG, "energy scan on channel %u failed (no result, or the radio reset)", g_zigbee.scan_channel);
    }
    if (g_zigbee.start_ran) {
        ziggurat_radio_energy_scan_done(max_rssi);
    }
}

// The radio went away (GONE, an unbind or loss, a reinit): transmits and scans in flight fail, the radio is cleared,
// the state is no_radio with `reason`. Idempotent: the GONE the mux posts and the input that follows it both land here.
static void handle_gone(const char *reason)
{
    zigbee_t *z = &g_zigbee;

    zigbee_spinel_on_gone(z->spinel);
    zigbee_status_set_state(ZIGBEE_STATE_NO_RADIO, reason);
}

// BIND or BACK: the channel's events go into our queue, then ATTACH behind the frames that came before the bind (they
// meet a client with no radio and are ignored), which runs the RCP's attach sequence.
static void bind_radio(const radio_mux_input_t *in)
{
    zigbee_t *z = &g_zigbee;
    const radio_mux_ev_t ev = {.type = ZIGBEE_EV_ATTACH};

    if (!radio_mux_chan_frames(in->ch)) { // never on the C6; a dongle in ziggurat gets frames (its declaration)
        ESP_LOGE(TAG, "%s delivers raw bytes: not used", radio_mux_chan_id(in->ch));
        return;
    }
    atomic_store(&z->ch, in->ch);
    zigbee_status_note_channel(radio_mux_chan_id(in->ch));
    if (radio_mux_bind_queue(in->ch, z->q) != ESP_OK) {
        ESP_LOGW(TAG, "%s: bound again meanwhile: the newer input follows", radio_mux_chan_id(in->ch));
        return;
    }
    if (xQueueSend(z->q, &ev, 0) != pdTRUE) { // the queue has room for every frame the pool holds, plus ours
        ESP_LOGW(TAG, "event queue full: attaching now");
        handle_event(&ev);
    }
}

// The orchestrator's latest input (on_input), acted on once: the latest wins and an older one is never seen. Every
// input kicks the task, so the queue's order holds: a GONE posted before the input is handled first. true: the radio
// went away (op_wait's "an event was delivered").
static bool take_input(void)
{
    zigbee_t *z = &g_zigbee;
    radio_mux_input_t in;
    char why[RADIO_MUX_REASON_MAX];

    taskENTER_CRITICAL(&z->in_lock);
    in = z->in;
    taskEXIT_CRITICAL(&z->in_lock);
    if (!zigbee_input_newer(z->in_seen, in.seq)) {
        return false;
    }
    z->in_seen = in.seq;
    switch (in.type) {
    case RADIO_MUX_IN_BIND:
    case RADIO_MUX_IN_BACK:
        bind_radio(&in);
        return false;
    case RADIO_MUX_IN_REINIT: // always accepted: the client stays, its transmits fail fast while the radio is paused
        snprintf(why, sizeof(why), "radio reinit: %.80s", in.reason);
        ESP_LOGI(TAG, "%s", why);
        handle_gone(why);
        radio_mux_reinit_reply(in.ch, in.seq, ESP_OK, NULL);
        break;
    default: // UNBIND, LOST
        handle_gone("radio gone");
        break;
    }
    ZIGBEE_STATUS(g_zigbee_status.counters.rcp_resets = zigbee_spinel_resets(z->spinel));
    return true;
}

// One item of the zigbee task's queue, from the loop or from op_wait. true: a radio event was given to the client
// (op_wait's "an event was delivered"); false: a KICK.
static bool handle_event(const radio_mux_ev_t *ev)
{
    zigbee_t *z = &g_zigbee;
    zigbee_spinel_t *sp = z->spinel;

    switch ((int)ev->type) { // int: ZIGBEE_EV_* lie outside the enum
    case RADIO_MUX_EV_RX_FRAME:
        // The client parses the slot in place and copies what it keeps; the slot goes back before the next item of
        // this loop (an RCP reset seen in this frame runs the attach sequence first, holding this one slot).
        zigbee_spinel_on_frame(sp, ev->slot, ev->len, ev->rx_us);
        radio_mux_release(atomic_load(&z->ch), ev->slot);
        break;
    case RADIO_MUX_EV_RESET:
        if (zigbee_spinel_has_radio(sp)) {
            ESP_LOGI(TAG, "radio reset");
            zigbee_status_set_state(ZIGBEE_STATE_STARTING, "radio resetting");
        }
        zigbee_spinel_on_reset(sp);
        break;
    case RADIO_MUX_EV_GONE:
        ESP_LOGW(TAG, "radio gone: running without a radio");
        handle_gone("radio gone");
        break;
    case ZIGBEE_EV_ATTACH:
        ESP_LOGI(TAG, "radio attached: %s", radio_mux_chan_id(atomic_load(&z->ch)));
        zigbee_status_set_state(ZIGBEE_STATE_STARTING, NULL);
        zigbee_spinel_attach(sp);
        break;
    default: // ZIGBEE_EV_KICK: taken out of the queue, so the next post queues a new one; the mailbox is read after
        zigbee_kick_taken(&z->kick);
        return false;
    }
    ZIGBEE_STATUS(g_zigbee_status.counters.rcp_resets = zigbee_spinel_resets(sp));
    return true;
}

/* ---- the link (zigbee task) ---- */

static void on_connect(const zigbee_msg_t *m)
{
    zigbee_t *z = &g_zigbee;
    const uint32_t old_sid = z->link_sid;
    const bool had_client = zigbee_link_client(&z->link) >= 0;
    const int to_close = zigbee_link_connect(&z->link, m->fd);
    const int64_t now = zigbee_now_epoch();
    char old_addr[ZIGBEE_ADDRESS_MAX];

    strlcpy(old_addr, z->link_addr, sizeof(old_addr));
    z->link_sid = m->sid;
    strlcpy(z->link_addr, m->frame ? (const char *)m->frame->data : "?", sizeof(z->link_addr));
    publish_link();
    if (to_close >= 0) { // the newest client takes over; the old socket's late frames are dropped by its generation
        const zigbee_out_t close = {.kind = ZIGBEE_OUT_CLOSE, .fd = to_close, .sid = old_sid};

        if (!zigbee_out_post(&close)) {
            ESP_LOGW(TAG, "could not close the replaced client %d", to_close);
        }
    }
    ESP_LOGI(TAG, "client %s connected (socket %d)%s", z->link_addr, m->fd, had_client ? ", replacing the previous" : "");
    taskENTER_CRITICAL(&g_zigbee_status_lock);
    if (had_client) {
        g_zigbee_status.has_last_client = true;
        strlcpy(g_zigbee_status.last_client_address, old_addr, sizeof(g_zigbee_status.last_client_address));
        g_zigbee_status.last_client_left = now;
    }
    g_zigbee_status.has_client = true;
    strlcpy(g_zigbee_status.client_address, z->link_addr, sizeof(g_zigbee_status.client_address));
    g_zigbee_status.client_since = now;
    taskEXIT_CRITICAL(&g_zigbee_status_lock);
    if (zigbee_link_take_send_hello(&z->link) && z->hello != NULL) {
        const zigbee_out_t hello = {
            .kind = ZIGBEE_OUT_FRAME,
            .counted = false,
            .fd = m->fd,
            .sid = m->sid,
            .gen = zigbee_link_generation(&z->link), // the generation of this client, at the moment it is queued
            .frame = zigbee_frame_copy(z->hello->data, z->hello->len),
        };

        if (hello.frame == NULL || !zigbee_out_post(&hello)) {
            ESP_LOGW(TAG, "could not queue the hello for client %s", z->link_addr);
        }
    }
}

static void on_disconnect(const zigbee_msg_t *m)
{
    zigbee_t *z = &g_zigbee;

    // A late disconnect of an old socket whose fd a newer client reuses has another session: it changes nothing.
    if (m->sid != z->link_sid || !zigbee_link_disconnect(&z->link, m->fd)) {
        return;
    }
    const int64_t now = zigbee_now_epoch();

    z->link_sid = 0;
    publish_link(); // the sender now discards the frames still queued for this client, which frees their slots
    ESP_LOGI(TAG, "client %s left", z->link_addr);
    taskENTER_CRITICAL(&g_zigbee_status_lock);
    g_zigbee_status.has_client = false;
    g_zigbee_status.has_last_client = true;
    strlcpy(g_zigbee_status.last_client_address, z->link_addr, sizeof(g_zigbee_status.last_client_address));
    g_zigbee_status.last_client_left = now;
    taskEXIT_CRITICAL(&g_zigbee_status_lock);
}

static void on_frame(const zigbee_msg_t *m)
{
    zigbee_t *z = &g_zigbee;

    if (m->sid == z->link_sid && m->frame != NULL &&
            zigbee_link_accept_inbound(&z->link, m->fd, m->frame->len, true)) {
        ZIGBEE_STATUS(g_zigbee_status.counters.frames_in++);
        ziggurat_host_frame(m->frame->data, m->frame->len);
    }
}

/* ---- the loop (zigbee task) ---- */

static void start_if_due(void)
{
    zigbee_t *z = &g_zigbee;

    if (!z->start_due || z->start_ran) {
        return;
    }
    z->start_due = false;
    z->start_ran = true;
    const int64_t now = zigbee_now_epoch();

    taskENTER_CRITICAL(&g_zigbee_status_lock);
    g_zigbee_status.stack_started = true;
    g_zigbee_status.started = now;
    taskEXIT_CRITICAL(&g_zigbee_status_lock);
    ESP_LOGI(TAG, "starting ziggurat");
    ziggurat_start();
    atomic_store(&z->started, true); // the WebSocket accepts clients from now on
}

// Polls ziggurat when its pender asked for it since the last poll.
static void process_if_woken(void)
{
    if (g_zigbee.start_ran && take_wake()) {
        ziggurat_process();
    }
}

// The mailbox: flags, the frames the sender took, then up to MAILBOX_BATCH messages in order, then the ready signal
// and ziggurat's wake. Only from the loop, never inside a ziggurat call.
//
// ziggurat_host_frame is a try_send into ziggurat's 16-deep INBOUND channel that drops the frame when it is full and
// reports nothing. So ziggurat is polled before the first message (a wake left from the last round: frames it has
// not taken yet) and after every message that woke it (handing a frame to a waiting ziggurat_main wakes it): its
// frame loop takes each frame before the next one is handed in. A frame it takes but cannot finish at once (a radio
// operation, or replies waiting for the client to read) leaves the next ones queued in INBOUND, up to its depth.
static void run_mailbox(void)
{
    zigbee_t *z = &g_zigbee;

    // The KICK claim is not released here: only when a KICK leaves the queue (handle_event), so a KICK still queued
    // behind the item just taken is never doubled. What op_wait swallowed is read now.
    z->kick_swallowed = false;
    z->mailbox_more = false;
    take_input();
    const uint32_t flags = take_flags();

    for (unsigned n = atomic_exchange(&z->sent, 0); n > 0; n--) {
        zigbee_link_sent(&z->link);
    }
    if (flags & ZIGBEE_FLAG_TICK) {
        zigbee_spinel_on_tick(z->spinel);
    }
    if (z->start_ran && (flags & ZIGBEE_FLAG_TIMER)) {
        zigbee_platform_timer_due();
    }
    if (z->start_ran && (flags & ZIGBEE_FLAG_WAKE)) {
        ziggurat_process();
    }
    process_if_woken();
    zigbee_msg_t m;
    unsigned n = 0;

    while (n < MAILBOX_BATCH && take_msg(&m)) {
        n++;
        if (z->start_ran) {
            switch (m.kind) {
            case ZIGBEE_MSG_CONNECT:
                on_connect(&m);
                break;
            case ZIGBEE_MSG_DISCONNECT:
                on_disconnect(&m);
                break;
            case ZIGBEE_MSG_FRAME:
                on_frame(&m);
                break;
            }
        }
        zigbee_frame_free(m.frame);
        process_if_woken();
    }
    if (n == MAILBOX_BATCH) {
        taskENTER_CRITICAL(&z->mb_lock);
        z->mailbox_more = z->mb_count > 0;
        taskEXIT_CRITICAL(&z->mb_lock);
    }
    if (z->start_ran && zigbee_link_drained(&z->link)) {
        ziggurat_host_send_ready();
    }
    process_if_woken();
}

static void zigbee_task(void *arg)
{
    zigbee_t *z = &g_zigbee;

    for (;;) {
        radio_mux_ev_t ev;
        const TickType_t wait = z->kick_swallowed || z->mailbox_more ? 0 : portMAX_DELAY;

        if (xQueueReceive(z->q, &ev, wait) == pdTRUE) {
            handle_event(&ev);
        }
        start_if_due();
        run_mailbox();
        start_if_due();
    }
}

/* ---- the orchestrator's instance (owner task) ---- */

static void tick_cb(void *arg)
{
    zigbee_post_flags(ZIGBEE_FLAG_TICK);
}

// Everything the zigbee task needs, made by the first bind (so a board with no ziggurat channel spends nothing on it).
// Each step is skipped when an earlier, partly failed try made it already.
static esp_err_t ensure_up(void)
{
    zigbee_t *z = &g_zigbee;

    if (atomic_load(&z->up)) {
        return ESP_OK;
    }
    if (z->spinel == NULL) {
        static const zigbee_spinel_ops_t ops = {
            .send = op_send,
            .request_reset = op_request_reset,
            .now_us = op_now_us,
            .wait = op_wait,
        };
        static const zigbee_spinel_sink_t sink = {
            .ready = sink_ready,
            .rx = sink_rx,
            .tx_done = sink_tx_done,
            .scan_done = sink_scan_done,
        };

        z->spinel = heap_caps_calloc(1, sizeof(*z->spinel), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        ESP_RETURN_ON_FALSE(z->spinel, ESP_ERR_NO_MEM, TAG, "spinel client");
        zigbee_spinel_init(z->spinel, &ops, &sink);
        zigbee_link_init(&z->link, ZIGBEE_LINK_QUEUE_DEPTH);
        publish_link();
    }
    if (z->q == NULL) {
        z->q = xQueueCreate(ZIGBEE_EVENT_DEPTH, sizeof(radio_mux_ev_t));
        ESP_RETURN_ON_FALSE(z->q, ESP_ERR_NO_MEM, TAG, "event queue");
    }
    if (z->out == NULL) {
        z->out = xQueueCreate(ZIGBEE_OUT_DEPTH, sizeof(zigbee_out_t));
        ESP_RETURN_ON_FALSE(z->out, ESP_ERR_NO_MEM, TAG, "outbound queue");
    }
    if (z->job_done == NULL) {
        z->job_done = xSemaphoreCreateBinary();
        ESP_RETURN_ON_FALSE(z->job_done, ESP_ERR_NO_MEM, TAG, "sender semaphore");
    }
    if (z->timer == NULL) {
        const esp_timer_create_args_t args = {.callback = zigbee_platform_timer_cb, .name = "zigbee"};

        ESP_RETURN_ON_ERROR(esp_timer_create(&args, &z->timer), TAG, "timer");
    }
    if (z->tick == NULL) {
        const esp_timer_create_args_t args = {.callback = tick_cb, .name = "zigbee_tick"};

        ESP_RETURN_ON_ERROR(esp_timer_create(&args, &z->tick), TAG, "tick timer");
        ESP_RETURN_ON_ERROR(esp_timer_start_periodic(z->tick, TICK_US), TAG, "tick start");
    }
    ESP_RETURN_ON_ERROR(zigbee_ws_start_sender(), TAG, "sender task");
    static TaskHandle_t task;

    if (task == NULL) {
        ESP_RETURN_ON_FALSE(xTaskCreatePinnedToCore(zigbee_task, "zigbee", ZIGBEE_TASK_STACK, NULL, ZIGBEE_TASK_PRIORITY,
                                                    &task, ZIGBEE_TASK_CORE) == pdPASS,
                            ESP_ERR_NO_MEM, TAG, "zigbee task");
    }
    atomic_store(&z->up, true);
    return ESP_OK;
}

// Owner task: the input is kept as the latest (the zigbee task acts on it) and the task is kicked; never blocks. The
// first bind makes the task and what it needs.
static void on_input(const radio_mux_input_t *in, void *ctx)
{
    zigbee_t *z = &g_zigbee;

    if ((in->type == RADIO_MUX_IN_BIND || in->type == RADIO_MUX_IN_BACK) && ensure_up() != ESP_OK) {
        ESP_LOGE(TAG, "%s: the zigbee task could not be made: the radio is not used", radio_mux_chan_id(in->ch));
    }
    taskENTER_CRITICAL(&z->in_lock);
    z->in = *in;
    taskEXIT_CRITICAL(&z->in_lock);
    zigbee_kick();
}

esp_err_t zigbee_register(void)
{
    ESP_RETURN_ON_FALSE(!s_registered, ESP_ERR_INVALID_STATE, TAG, "already registered");
    zigbee_platform_boot();
    const radio_mux_instance_t inst = {
        .mode = RADIO_MUX_MODE_ZIGGURAT,
        .needs = RADIO_MUX_FW_SPINEL,
        .limit = 1,
        .reboot_on_change = true, // ziggurat cannot be stopped: binding or unbinding it reboots
        .frames = true,
        .input = on_input,
    };

    ESP_RETURN_ON_ERROR(radio_mux_register_instance(&inst), TAG, "registering the ziggurat instance");
    s_registered = true;
    return ESP_OK;
}
