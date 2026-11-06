// The radio channel mux at runtime: the channel registry and its NVS, the consumers, the owner task (plug-in,
// detection, attach, unplug, settings) and the delivery of events and frames (spec section 3).
//
// Only the owner task changes a channel or a transport's attach state, opens or closes a device and calls a consumer's
// attach or detach. The transports post messages to it and deliver received frames themselves without waiting. The
// API calls post a message and wait for the owner's answer.
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "board_cores.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_eventfd.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "radio_mux_priv.h"

static const char *TAG = "radio_mux";

#define OWNER_PRIORITY 6
#define OWNER_STACK 6144
#define QUEUE_LEN 12
#define CALL_WAIT_MS 5000
#define DETECT_POLL_MS 10
#define IDLE_POLL_MS 1000
#define PRESENCE_POLL_US 1000000 // how often a device that is not open is checked for still being there
#define DETECT_CHUNK 256
#define RX_RING_SIZE (16 * 1024) // a 100 ms burst at 921600 baud stays well inside
#define EVQ_LEN (RADIO_MUX_POOL_SLOTS + 4) // frames hold a slot each, so control events always fit
#define DEFAULT_BAUD 460800
#define EPOCH_VALID 1577836800 // 2020-01-01: an earlier clock was never set

#define NVS_NAMESPACE "radio_mux"
#define NVS_INDEX "chans"
#define BLOB_VERSION 1

// At least one tick: at CONFIG_FREERTOS_HZ 100, pdMS_TO_TICKS(10) is 1 and smaller waits would not block.
#define WAIT_TICKS(ms) (pdMS_TO_TICKS(ms) > 0 ? pdMS_TO_TICKS(ms) : 1)

struct radio_mux_chan {
    bool used;
    bool remembered; // stored in NVS (unsupported devices never are)
    char id[RADIO_MUX_ID_MAX];
    radio_mux_kind_t kind;
    mux_port_t *port; // the transport of its kind (for the whole firmware life)
    // Stored settings.
    radio_mux_mode_t mode;
    uint32_t baud; // 0 = none stored (DEFAULT_BAUD applies)
    bool flow;
    radio_mux_proxy_settings_t proxy;
    radio_mux_firmware_t fw;
    int64_t first_seen, last_seen;
    mux_usb_dev_t usb;
    char label[RADIO_MUX_LABEL_MAX];
    // Runtime, published through the snapshot.
    bool present, supported;
    radio_mux_mode_t eff_mode;
    radio_mux_state_t state;
    char reason[RADIO_MUX_REASON_MAX];
    radio_mux_counters_t ctr; // while not on its port
};

// Version 1 of a channel's NVS blob. Read back by the same firmware layout; another version is ignored (defaults).
typedef struct {
    uint8_t version;
    uint8_t kind;
    uint8_t mode;
    uint8_t flow;
    uint32_t baud;
    radio_mux_proxy_settings_t proxy;
    radio_mux_fw_record_t fw;
    uint8_t fw_source;
    int64_t fw_at;
    int64_t first_seen, last_seen;
    uint16_t vid, pid;
    uint8_t driver;
    char serial[RADIO_MUX_SERIAL_MAX];
    char label[RADIO_MUX_LABEL_MAX];
    char id[RADIO_MUX_ID_MAX];
} chan_blob_t;

struct mux_call {
    SemaphoreHandle_t done;
    atomic_int refs; // the caller and the owner; the last one frees
    char id[RADIO_MUX_ID_MAX];
    radio_mux_set_req_t req;
    bool have_mover;
    radio_mux_verdict_t verdict;
    esp_err_t err;
    char from[RADIO_MUX_ID_MAX];
};

static QueueHandle_t s_queue;
static SemaphoreHandle_t s_lock; // the channel records: written by the owner, read by the API and consumers
static struct radio_mux_chan s_chans[RADIO_MUX_CHANNELS_MAX];
static mux_port_t s_c6_port;
static mux_port_t s_usb_port;
static radio_mux_consumer_t s_consumers[RADIO_MUX_RULES_MAX];
static size_t s_n_consumers;
static radio_mux_rules_t s_rules;
static atomic_bool s_started;
static esp_err_t (*_Atomic s_mover)(const char *from, const char *to);

static void feed(mux_port_t *p, radio_mux_attach_ev_t ev);

/* ---- small helpers ---- */

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static int64_t now_epoch(void)
{
    const time_t t = time(NULL);

    return t >= EPOCH_VALID ? (int64_t)t : 0;
}

static void lock(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGive(s_lock);
}

static uint32_t eff_baud(const struct radio_mux_chan *ch)
{
    return ch->baud ? ch->baud : DEFAULT_BAUD;
}

static const radio_mux_consumer_t *find_consumer(radio_mux_mode_t mode)
{
    for (size_t i = 0; i < s_n_consumers; i++) {
        if (s_consumers[i].mode == mode) {
            return &s_consumers[i];
        }
    }
    return NULL;
}

static struct radio_mux_chan *find_chan(const char *id)
{
    for (size_t i = 0; i < RADIO_MUX_CHANNELS_MAX; i++) {
        if (s_chans[i].used && strcmp(s_chans[i].id, id) == 0) {
            return &s_chans[i];
        }
    }
    return NULL;
}

static void poke(mux_port_t *p)
{
    const uint64_t one = 1;

    if (p->efd >= 0 && write(p->efd, &one, sizeof(one)) != sizeof(one)) {
        ESP_LOGW(TAG, "eventfd write failed");
    }
}

static size_t ring_waiting(mux_port_t *p)
{
    UBaseType_t waiting = 0;

    if (p->ring) {
        vRingbufferGetInfo(p->ring, NULL, NULL, NULL, NULL, &waiting);
    }
    return (size_t)waiting;
}

static size_t ring_take(mux_port_t *p, uint8_t *out, size_t cap)
{
    size_t total = 0;

    // A byte ring hands out at most the run up to its end: a second call takes the wrapped part.
    for (int i = 0; i < 2 && p->ring && total < cap; i++) {
        size_t got = 0;
        uint8_t *d = xRingbufferReceiveUpTo(p->ring, &got, 0, cap - total);

        if (!d) {
            break;
        }
        memcpy(out + total, d, got);
        vRingbufferReturnItem(p->ring, d);
        total += got;
    }
    return total;
}

static void ring_flush(mux_port_t *p)
{
    size_t got;
    void *d;

    while (p->ring && (d = xRingbufferReceiveUpTo(p->ring, &got, 0, RX_RING_SIZE)) != NULL) {
        vRingbufferReturnItem(p->ring, d);
    }
}

// Clears the eventfd, then sets it again when something is still waiting: an event or bytes that arrived between the
// last take and the clear are never left without a wakeup.
static void rearm(mux_port_t *p)
{
    uint64_t v;

    if (p->efd < 0 || read(p->efd, &v, sizeof(v)) != sizeof(v)) {
        return;
    }
    if (uxQueueMessagesWaiting(p->evq) > 0 || ring_waiting(p) > 0) {
        poke(p);
    }
}

static void slot_put(mux_port_t *p, uint8_t *frame)
{
    if (!p->pool.slots || !frame) {
        return;
    }
    // The frame sits at an offset inside its slot (R12): back to the slot start.
    const uintptr_t base = (uintptr_t)p->pool.slots, at = (uintptr_t)frame;
    uint8_t *slot = frame;

    if (at >= base && at < base + p->pool.n * p->pool.slot_size) {
        slot = p->pool.slots + ((at - base) / p->pool.slot_size) * p->pool.slot_size;
    }
    taskENTER_CRITICAL(&p->lock);
    radio_mux_pool_put(&p->pool, slot);
    taskEXIT_CRITICAL(&p->lock);
}

static uint8_t *slot_take(mux_port_t *p)
{
    uint8_t *slot = NULL;

    if (p->pool.slots) {
        taskENTER_CRITICAL(&p->lock);
        slot = radio_mux_pool_take(&p->pool);
        taskEXIT_CRITICAL(&p->lock);
    }
    return slot;
}

/* ---- delivery (transport callbacks and the owner, rx_lock held) ---- */

static bool post_ev(mux_port_t *p, const radio_mux_ev_t *ev)
{
    QueueHandle_t q = p->bound ? p->bound : p->evq;
    const bool sent = q && xQueueSend(q, ev, 0) == pdTRUE;

    if (sent && !p->bound) {
        poke(p);
    }
    return sent;
}

static void deliver_frame(mux_port_t *p, uint8_t *frame, size_t len)
{
    const radio_mux_ev_t ev = {.type = RADIO_MUX_EV_RX_FRAME, .slot = frame, .len = len, .rx_us = esp_timer_get_time()};
    const bool sent = post_ev(p, &ev);

    taskENTER_CRITICAL(&p->lock);
    const radio_mux_post_t r = radio_mux_post_rx(!sent, len, &p->ctr);
    taskEXIT_CRITICAL(&p->lock);
    if (r == RADIO_MUX_DROPPED) {
        slot_put(p, frame);
    }
}

// The decoder's next target: a fresh slot, or the discard buffer while the pool is empty (the frame is still parsed,
// so the stream stays in sync, and counted as a drop when it completes).
static void next_dec_buf(mux_port_t *p)
{
    p->dec_slot = slot_take(p);
    radio_mux_hdlc_dec_set_buf(&p->dec, p->dec_slot ? p->dec_slot + MUX_SLOT_HDR : p->discard,
                               MUX_SLOT_SIZE - MUX_SLOT_HDR);
}

void mux_rx_bytes(mux_port_t *p, const uint8_t *data, size_t len)
{
    switch (p->rx_mode) {
    case MUX_RX_DISCARD:
        return;
    case MUX_RX_DETECT:
        xRingbufferSend(p->ring, data, len, 0); // the detection takes what fits
        return;
    case MUX_RX_RAW: {
        const bool kept = xRingbufferSend(p->ring, data, len, 0) == pdTRUE;

        taskENTER_CRITICAL(&p->lock);
        if (kept) {
            p->ctr.bytes_in += len;
        } else {
            p->ctr.drops++;
        }
        taskEXIT_CRITICAL(&p->lock);
        if (kept) {
            poke(p);
        }
        return;
    }
    case MUX_RX_FRAMES:
        break;
    }
    while (len > 0) {
        bool done = false;
        const size_t used = radio_mux_hdlc_decode_feed(&p->dec, data, len, &done);

        data += used;
        len -= used;
        if (!done) {
            continue;
        }
        if (p->dec_slot) {
            deliver_frame(p, p->dec_slot + MUX_SLOT_HDR, p->dec.len);
        } else {
            taskENTER_CRITICAL(&p->lock);
            p->ctr.drops++;
            taskEXIT_CRITICAL(&p->lock);
        }
        next_dec_buf(p);
    }
    const uint32_t errors = p->dec.crc_errors + p->dec.overruns;

    if (errors != p->dec_errors) {
        taskENTER_CRITICAL(&p->lock);
        p->ctr.rx_errors += errors - p->dec_errors;
        taskEXIT_CRITICAL(&p->lock);
        p->dec_errors = errors;
    }
}

// Owner: switches what the receive path does with incoming bytes; waits for a callback in progress to finish.
static void set_rx(mux_port_t *p, mux_rx_mode_t mode)
{
    xSemaphoreTake(p->rx_lock, portMAX_DELAY);
    if (p->dec_slot) {
        slot_put(p, p->dec_slot + MUX_SLOT_HDR);
        p->dec_slot = NULL;
    }
    ring_flush(p);
    p->rx_mode = mode;
    if (mode == MUX_RX_FRAMES) {
        p->dec_slot = slot_take(p);
        radio_mux_hdlc_dec_init(&p->dec, p->dec_slot ? p->dec_slot + MUX_SLOT_HDR : p->discard,
                                MUX_SLOT_SIZE - MUX_SLOT_HDR);
        p->dec_errors = 0;
    }
    xSemaphoreGive(p->rx_lock);
}

// Owner: GONE or RESET to the attached consumer. The channel's own queue always has room for it (EVQ_LEN); a bound
// queue the consumer sized too small loses it, and the detach that follows GONE still tells the consumer.
static void post_ctl(mux_port_t *p, radio_mux_ev_type_t type)
{
    const radio_mux_ev_t ev = {.type = type, .slot = NULL, .len = 0, .rx_us = esp_timer_get_time()};

    xSemaphoreTake(p->rx_lock, portMAX_DELAY);
    const bool sent = post_ev(p, &ev);

    xSemaphoreGive(p->rx_lock);
    if (!sent) {
        ESP_LOGE(TAG, "%s: event %d lost, the consumer's queue is full", p->chan ? p->chan->id : "?", (int)type);
    }
}

// Owner, before an attach: what a previous consumer left in the channel's own queue goes, its slots back to the pool.
static void drain_evq(mux_port_t *p)
{
    radio_mux_ev_t ev;

    while (xQueueReceive(p->evq, &ev, 0) == pdTRUE) {
        if (ev.type == RADIO_MUX_EV_RX_FRAME) {
            slot_put(p, ev.slot);
        }
    }
    rearm(p);
}

/* ---- NVS ---- */

static void blob_from_chan(const struct radio_mux_chan *ch, chan_blob_t *b)
{
    memset(b, 0, sizeof(*b));
    b->version = BLOB_VERSION;
    b->kind = (uint8_t)ch->kind;
    b->mode = (uint8_t)ch->mode;
    b->flow = ch->flow ? 1 : 0;
    b->baud = ch->baud;
    b->proxy = ch->proxy;
    b->fw = ch->fw.rec;
    b->fw_source = (uint8_t)ch->fw.source;
    b->fw_at = ch->fw.at;
    b->first_seen = ch->first_seen;
    b->last_seen = ch->last_seen;
    b->vid = ch->usb.vid;
    b->pid = ch->usb.pid;
    b->driver = (uint8_t)ch->usb.driver;
    memcpy(b->serial, ch->usb.serial, sizeof(b->serial));
    memcpy(b->label, ch->label, sizeof(b->label));
    memcpy(b->id, ch->id, sizeof(b->id));
}

// A blob that is malformed or out of range keeps the channel's defaults for that part.
static void chan_from_blob(struct radio_mux_chan *ch, const chan_blob_t *b)
{
    const radio_mux_mode_t mode = (radio_mux_mode_t)b->mode;

    if (mode == RADIO_MUX_MODE_OFF || mode == RADIO_MUX_MODE_OTBR || mode == RADIO_MUX_MODE_ZIGGURAT ||
            mode == RADIO_MUX_MODE_RCP_PROXY) {
        ch->mode = mode;
    }
    ch->baud = radio_mux_baud_ok(b->baud) ? b->baud : 0;
    ch->flow = b->flow != 0;
    if (radio_mux_proxy_ok(&b->proxy)) {
        ch->proxy = b->proxy;
    }
    if (b->fw_source == RADIO_MUX_FW_SRC_PROBE || b->fw_source == RADIO_MUX_FW_SRC_PASSIVE) {
        ch->fw.rec = b->fw;
        ch->fw.rec.version[sizeof(ch->fw.rec.version) - 1] = '\0';
        ch->fw.rec.chip[sizeof(ch->fw.rec.chip) - 1] = '\0';
        ch->fw.source = (radio_mux_fw_source_t)b->fw_source;
        ch->fw.at = b->fw_at;
    }
    ch->first_seen = b->first_seen;
    ch->last_seen = b->last_seen;
    ch->usb.vid = b->vid;
    ch->usb.pid = b->pid;
    ch->usb.driver = b->driver <= RADIO_MUX_DRV_CDC_ACM ? (radio_mux_driver_t)b->driver : RADIO_MUX_DRV_NONE;
    memcpy(ch->usb.serial, b->serial, sizeof(ch->usb.serial));
    ch->usb.serial[sizeof(ch->usb.serial) - 1] = '\0';
    memcpy(ch->label, b->label, sizeof(ch->label));
    ch->label[sizeof(ch->label) - 1] = '\0';
}

// The index: the ids of every remembered channel, RADIO_MUX_ID_MAX bytes each.
static esp_err_t store_index(nvs_handle_t nvs)
{
    char ids[RADIO_MUX_CHANNELS_MAX][RADIO_MUX_ID_MAX];
    size_t n = 0;

    memset(ids, 0, sizeof(ids));
    for (size_t i = 0; i < RADIO_MUX_CHANNELS_MAX; i++) {
        if (s_chans[i].used && s_chans[i].remembered) {
            memcpy(ids[n++], s_chans[i].id, RADIO_MUX_ID_MAX);
        }
    }
    return n ? nvs_set_blob(nvs, NVS_INDEX, ids, n * RADIO_MUX_ID_MAX) : nvs_erase_key(nvs, NVS_INDEX);
}

// Owner. Writes the channel's blob, and the index when the channel is new to NVS.
static esp_err_t store_chan(struct radio_mux_chan *ch)
{
    nvs_handle_t nvs;
    char key[16];
    chan_blob_t b;

    ESP_RETURN_ON_ERROR(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs), TAG, "nvs_open");
    radio_mux_nvs_key(ch->id, key);
    blob_from_chan(ch, &b);
    esp_err_t err = nvs_set_blob(nvs, key, &b, sizeof(b));

    if (err == ESP_OK && !ch->remembered) {
        ch->remembered = true;
        err = store_index(nvs);
        if (err != ESP_OK) {
            ch->remembered = false;
        }
    }
    err = err == ESP_OK ? nvs_commit(nvs) : err;
    nvs_close(nvs);
    ESP_RETURN_ON_ERROR(err, TAG, "storing %s", ch->id);
    return ESP_OK;
}

// Owner. Erases the channel's blob and drops it from the index.
static esp_err_t erase_chan(struct radio_mux_chan *ch)
{
    nvs_handle_t nvs;
    char key[16];

    if (!ch->remembered) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs), TAG, "nvs_open");
    radio_mux_nvs_key(ch->id, key);
    esp_err_t err = nvs_erase_key(nvs, key);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    ch->remembered = false;
    err = err == ESP_OK ? store_index(nvs) : err;
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK; // erasing an index that was already gone
    }
    err = err == ESP_OK ? nvs_commit(nvs) : err;
    nvs_close(nvs);
    ESP_RETURN_ON_ERROR(err, TAG, "erasing %s", ch->id);
    return ESP_OK;
}

static void chan_defaults(struct radio_mux_chan *ch, const char *id, radio_mux_kind_t kind)
{
    memset(ch, 0, sizeof(*ch));
    ch->used = true;
    snprintf(ch->id, sizeof(ch->id), "%s", id);
    ch->kind = kind;
    ch->port = kind == RADIO_MUX_KIND_C6 ? &s_c6_port : &s_usb_port;
    ch->mode = kind == RADIO_MUX_KIND_C6 ? RADIO_MUX_MODE_OTBR : RADIO_MUX_MODE_OFF;
    radio_mux_proxy_defaults(&ch->proxy);
    ch->eff_mode = RADIO_MUX_MODE_UNAVAILABLE;
    ch->supported = true;
    snprintf(ch->reason, sizeof(ch->reason), "not detected");
}

static void load_chans(void)
{
    char ids[RADIO_MUX_CHANNELS_MAX][RADIO_MUX_ID_MAX];
    size_t len = sizeof(ids), slot = 1; // slot 0 is the C6
    nvs_handle_t nvs;
    chan_blob_t b;

    chan_defaults(&s_chans[0], "c6", RADIO_MUX_KIND_C6);
    // Until the C6 transport exists (Task 6) the mux lists c6 without opening it; br_core runs its own SPI path.
    snprintf(s_chans[0].reason, sizeof(s_chans[0].reason), "C6 transport not in this build yet");
    snprintf(s_chans[0].label, sizeof(s_chans[0].label), "ESP32-C6 (on board)");
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);

    if (err != ESP_OK) {
        if (err != ESP_ERR_NVS_NOT_FOUND) { // not found: nothing stored yet
            ESP_LOGE(TAG, "reading the channels failed (%s): defaults", esp_err_to_name(err));
        }
        return;
    }
    if (nvs_get_blob(nvs, NVS_INDEX, ids, &len) != ESP_OK) {
        len = 0;
    }
    for (size_t i = 0; i < len / RADIO_MUX_ID_MAX; i++) {
        char key[16];
        size_t blen = sizeof(b);

        ids[i][RADIO_MUX_ID_MAX - 1] = '\0';
        radio_mux_nvs_key(ids[i], key);
        if (nvs_get_blob(nvs, key, &b, &blen) != ESP_OK || blen != sizeof(b) || b.version != BLOB_VERSION ||
                strncmp(b.id, ids[i], RADIO_MUX_ID_MAX) != 0) {
            ESP_LOGW(TAG, "stored channel %s unreadable: skipped", ids[i]);
            continue;
        }
        const bool c6 = strcmp(ids[i], "c6") == 0;
        struct radio_mux_chan *ch = c6 ? &s_chans[0] : (slot < RADIO_MUX_CHANNELS_MAX ? &s_chans[slot++] : NULL);

        if (ch == NULL) {
            break;
        }
        if (!c6) {
            chan_defaults(ch, ids[i], RADIO_MUX_KIND_USB);
        }
        chan_from_blob(ch, &b);
        ch->remembered = true;
    }
    nvs_close(nvs);
}

/* ---- the channel view ---- */

static radio_mux_chan_view_t view_of(const struct radio_mux_chan *ch)
{
    return (radio_mux_chan_view_t) {
        .id = ch->id, .mode = ch->mode, .present = ch->present, .supported = ch->supported, .fw = ch->fw.rec.type,
    };
}

// R13: the stored mode's consumer attaches when it is registered, the channel present, its device open, and the
// firmware fits.
static bool want_attach(const mux_port_t *p)
{
    const struct radio_mux_chan *ch = p->chan;

    if (!ch || (p->kind == RADIO_MUX_KIND_USB && !usb_transport_is_open())) {
        return false;
    }
    const radio_mux_chan_view_t v = view_of(ch);

    return radio_mux_attach_wanted(&s_rules, &v, find_consumer(ch->mode) != NULL);
}

// Owner: what the API shows for the channel on port p (mode, state, reason), from the attach state.
static void refresh(mux_port_t *p)
{
    struct radio_mux_chan *ch = p->chan;

    if (!ch) {
        return;
    }
    lock();
    switch (p->att.state) {
    case RADIO_MUX_AS_PROBING:
    case RADIO_MUX_AS_REOPENING:
        ch->eff_mode = RADIO_MUX_MODE_OFF;
        ch->state = RADIO_MUX_STATE_STARTING;
        ch->reason[0] = '\0';
        break;
    case RADIO_MUX_AS_ATTACHED:
        ch->eff_mode = ch->mode;
        ch->state = RADIO_MUX_STATE_RUNNING;
        snprintf(ch->reason, sizeof(ch->reason), "%s", p->warn); // e.g. flow control unsupported
        break;
    case RADIO_MUX_AS_IDLE: {
        char why[RADIO_MUX_REASON_MAX];
        const radio_mux_mode_t m = radio_mux_after_detect(&s_rules, ch->mode, ch->fw.rec.type, why, sizeof(why));

        if (p->fault[0] != '\0') { // not open, or the attach failed
            ch->eff_mode = m;
            ch->state = RADIO_MUX_STATE_FAULT;
            snprintf(ch->reason, sizeof(ch->reason), "%s", p->fault);
        } else if (m == RADIO_MUX_MODE_OFF) { // stored off, or the firmware doesn't fit (why says so)
            ch->eff_mode = RADIO_MUX_MODE_OFF;
            ch->state = RADIO_MUX_STATE_IDLE;
            snprintf(ch->reason, sizeof(ch->reason), "%s", why);
        } else { // fits, but nothing runs the mode
            ch->eff_mode = m;
            ch->state = RADIO_MUX_STATE_FAULT;
            snprintf(ch->reason, sizeof(ch->reason), "no %s consumer in this build", radio_mux_mode_name(m));
        }
        break;
    }
    case RADIO_MUX_AS_ABSENT:
    case RADIO_MUX_AS_CLOSING:
        ch->eff_mode = RADIO_MUX_MODE_UNAVAILABLE;
        ch->state = RADIO_MUX_STATE_IDLE;
        snprintf(ch->reason, sizeof(ch->reason), "not detected");
        break;
    }
    unlock();
}

/* ---- owner: the device ---- */

// Opens the port's device for its channel at `baud`. A failure lands in p->fault (the device stays closed), a warning
// in p->warn.
static bool open_dev(mux_port_t *p, uint32_t baud)
{
    char msg[RADIO_MUX_REASON_MAX];
    const bool ok = usb_transport_open(p, &p->dev, baud, p->chan->flow, msg, sizeof(msg)) == ESP_OK;

    snprintf(ok ? p->warn : p->fault, RADIO_MUX_REASON_MAX, "%s", msg);
    if (ok) {
        p->fault[0] = '\0';
    }
    if (msg[0]) {
        ESP_LOGW(TAG, "%s: %s", p->chan->id, msg);
    }
    return ok;
}

// New line settings on the open device; a failure closes it (p->fault says why).
static bool line_dev(mux_port_t *p, uint32_t baud)
{
    char msg[RADIO_MUX_REASON_MAX];
    const bool ok = usb_transport_line(p, baud, p->chan->flow, msg, sizeof(msg)) == ESP_OK;

    snprintf(ok ? p->warn : p->fault, RADIO_MUX_REASON_MAX, "%s", msg);
    if (msg[0]) {
        ESP_LOGW(TAG, "%s: %s", p->chan->id, msg);
    }
    return ok;
}

static void write_dev(mux_port_t *p, const uint8_t *data, size_t n)
{
    if (n > 0) {
        xSemaphoreTake(p->tx_lock, portMAX_DELAY);
        usb_transport_write(data, n); // a failure shows as no answer; an unplug arrives as GONE
        xSemaphoreGive(p->tx_lock);
    }
}

/* ---- owner: the attach actions ---- */

// The receive path runs before the consumer's attach, so nothing it asks for in attach is lost: frames that arrive
// before it binds its queue wait in the channel's queue and move over in radio_mux_bind_queue.
static void do_attach(mux_port_t *p)
{
    struct radio_mux_chan *ch = p->chan;
    const radio_mux_consumer_t *c = find_consumer(ch->mode);

    if (!c) { // want_attach said yes, so this does not happen; keep the machine consistent anyway
        snprintf(p->fault, sizeof(p->fault), "no %s consumer", radio_mux_mode_name(ch->mode));
        feed(p, RADIO_MUX_AEV_ATTACH_FAILED);
        return;
    }
    drain_evq(p);
    xSemaphoreTake(p->rx_lock, portMAX_DELAY);
    p->bound = NULL;
    xSemaphoreGive(p->rx_lock);
    p->consumer = c;
    p->tx_frames = c->frames;
    set_rx(p, c->frames ? MUX_RX_FRAMES : MUX_RX_RAW);
    atomic_store(&p->tx_ok, true);
    const esp_err_t err = c->attach((radio_mux_chan_t *)ch, c->ctx);

    if (err != ESP_OK) {
        atomic_store(&p->tx_ok, false);
        set_rx(p, MUX_RX_DISCARD);
        xSemaphoreTake(p->rx_lock, portMAX_DELAY);
        p->bound = NULL;
        xSemaphoreGive(p->rx_lock);
        drain_evq(p);
        p->consumer = NULL;
        snprintf(p->fault, sizeof(p->fault), "%s attach failed: %s", radio_mux_mode_name(ch->mode),
                 esp_err_to_name(err));
        ESP_LOGW(TAG, "%s: %s", ch->id, p->fault);
        feed(p, RADIO_MUX_AEV_ATTACH_FAILED);
        return;
    }
    p->fault[0] = '\0';
    ESP_LOGI(TAG, "%s: %s attached", ch->id, radio_mux_mode_name(ch->mode));
}

static void do_detach(mux_port_t *p)
{
    const radio_mux_consumer_t *c = p->consumer;

    atomic_store(&p->tx_ok, false);
    set_rx(p, MUX_RX_DISCARD);
    if (!c) {
        return;
    }
    post_ctl(p, RADIO_MUX_EV_GONE);
    c->detach((radio_mux_chan_t *)p->chan, c->ctx);
    p->consumer = NULL;
    xSemaphoreTake(p->rx_lock, portMAX_DELAY);
    p->bound = NULL;
    xSemaphoreGive(p->rx_lock);
    ESP_LOGI(TAG, "%s: %s detached", p->chan->id, radio_mux_mode_name(c->mode));
}

// The device leaves the transport: closed once (the only close of this handle), the channel listed as unavailable.
static void close_device(mux_port_t *p)
{
    struct radio_mux_chan *ch = p->chan;

    atomic_store(&p->tx_ok, false);
    set_rx(p, MUX_RX_DISCARD);
    if (p->kind == RADIO_MUX_KIND_USB) {
        usb_transport_close(p);
    }
    if (!ch) {
        return;
    }
    lock();
    taskENTER_CRITICAL(&p->lock);
    ch->ctr = p->ctr;
    taskEXIT_CRITICAL(&p->lock);
    ch->present = false;
    ch->last_seen = now_epoch();
    ch->eff_mode = RADIO_MUX_MODE_UNAVAILABLE;
    ch->state = RADIO_MUX_STATE_IDLE;
    snprintf(ch->reason, sizeof(ch->reason), "not detected");
    p->chan = NULL;
    unlock();
    if (ch->remembered) {
        store_chan(ch);
    }
    ESP_LOGI(TAG, "%s: unplugged", ch->id);
}

static struct radio_mux_chan *evict_slot(void)
{
    struct radio_mux_chan *oldest = NULL;

    for (size_t i = 1; i < RADIO_MUX_CHANNELS_MAX; i++) {
        struct radio_mux_chan *ch = &s_chans[i];

        if (!ch->used) {
            return ch;
        }
        if (!ch->present && ch->port->chan != ch && (!oldest || ch->last_seen < oldest->last_seen)) {
            oldest = ch;
        }
    }
    if (oldest) {
        ESP_LOGW(TAG, "%s forgotten: room for a new channel", oldest->id);
        erase_chan(oldest);
        lock();
        oldest->used = false;
        unlock();
    }
    return oldest;
}

// The channel record of a device: found by id, or a new one (defaults, mode off).
static struct radio_mux_chan *chan_for_dev(const mux_usb_dev_t *dev)
{
    char id[RADIO_MUX_ID_MAX];

    radio_mux_channel_id(dev->vid, dev->pid, dev->serial[0] ? dev->serial : NULL, id);
    struct radio_mux_chan *ch = find_chan(id);

    if (ch) {
        return ch;
    }
    ch = evict_slot();
    if (!ch) {
        return NULL;
    }
    lock();
    chan_defaults(ch, id, RADIO_MUX_KIND_USB);
    ch->first_seen = now_epoch();
    unlock();
    return ch;
}

static void fill_usb(struct radio_mux_chan *ch, const mux_usb_dev_t *dev)
{
    const char *drv = radio_mux_driver_name(dev->driver);

    ch->usb = *dev;
    if (dev->product[0]) {
        snprintf(ch->label, sizeof(ch->label), "%s", dev->product);
    } else {
        snprintf(ch->label, sizeof(ch->label), "%s %04x:%04x", drv ? drv : "USB", dev->vid, dev->pid);
    }
}

// PROBE: the pending device takes the port in mode off and the detection starts.
static void start_probe(mux_port_t *p)
{
    struct radio_mux_chan *ch = chan_for_dev(&p->pending);
    const uint8_t *tx;
    size_t tx_n;
    radio_mux_fw_record_t unused;

    if (!ch) {
        ESP_LOGE(TAG, "%04x:%04x: no room in the channel list", p->pending.vid, p->pending.pid);
        feed(p, RADIO_MUX_AEV_UNPLUG); // back to ABSENT through CLOSING; nothing is open
        return;
    }
    p->dev = p->pending;
    p->fault[0] = '\0';
    p->warn[0] = '\0';
    lock();
    p->chan = ch;
    fill_usb(ch, &p->dev);
    ch->present = true;
    ch->supported = true;
    ch->last_seen = now_epoch();
    unlock();
    taskENTER_CRITICAL(&p->lock);
    p->ctr = ch->ctr;
    taskEXIT_CRITICAL(&p->lock);
    store_chan(ch); // a supported dongle is remembered from its first plug-in
    refresh(p);
    ESP_LOGI(TAG, "%s (%s): detecting the firmware", ch->id, ch->label);
    radio_mux_detect_start(&p->det, now_ms());
    radio_mux_detect_next(&p->det, now_ms(), NULL, 0, &p->det_baud, &tx, &tx_n, &p->det_wait_ms, &unused);
    if (!open_dev(p, p->det_baud)) {
        feed(p, RADIO_MUX_AEV_DETECT_DONE); // no detection without a device: the reopen tries once more
        return;
    }
    set_rx(p, MUX_RX_DETECT);
    write_dev(p, tx, tx_n);
}

static void detect_done(mux_port_t *p, const radio_mux_fw_record_t *rec)
{
    struct radio_mux_chan *ch = p->chan;

    lock();
    ch->fw.rec = *rec;
    ch->fw.source = RADIO_MUX_FW_SRC_PROBE;
    ch->fw.at = now_epoch();
    if (ch->baud == 0 && rec->baud != 0) { // the answering baud becomes the channel's when none was set
        ch->baud = rec->baud;
    }
    unlock();
    store_chan(ch);
    ESP_LOGI(TAG, "%s: firmware %s %s%s%s at %lu baud", ch->id, radio_mux_fw_name(rec->type), rec->version,
             rec->chip[0] ? ", " : "", rec->chip, (unsigned long)rec->baud);
    feed(p, RADIO_MUX_AEV_DETECT_DONE);
}

// Owner loop, while PROBING: hands the bytes received since the last call to the sequencer (R16).
static void detect_step(mux_port_t *p)
{
    uint8_t rx[DETECT_CHUNK];
    const size_t n = ring_take(p, rx, sizeof(rx));
    const int64_t now = now_ms();
    uint32_t baud;
    const uint8_t *tx;
    size_t tx_n;
    radio_mux_fw_record_t rec;

    if (n == 0 && now < p->det_wait_ms) {
        return;
    }
    if (radio_mux_detect_next(&p->det, now, rx, n, &baud, &tx, &tx_n, &p->det_wait_ms, &rec) == RADIO_MUX_PROBE_DONE) {
        set_rx(p, MUX_RX_DISCARD);
        detect_done(p, &rec);
        return;
    }
    // A new baud is new line coding on the open device: the "reopen at *baud" of the sequencer, without a close.
    if (baud != p->det_baud) {
        p->det_baud = baud;
        if (!line_dev(p, baud)) {
            set_rx(p, MUX_RX_DISCARD);
            feed(p, RADIO_MUX_AEV_DETECT_DONE); // detection stopped; the old firmware record stays
            return;
        }
    }
    if (p->det.new_step) {
        ring_flush(p); // a new probe step starts clean; a follow-up keeps what came
    }
    write_dev(p, tx, tx_n);
}

// REOPEN: the detection's bytes leave nothing behind; the device comes back at the channel's line settings.
static void reopen(mux_port_t *p)
{
    set_rx(p, MUX_RX_DISCARD);
    usb_transport_close(p);
    open_dev(p, eff_baud(p->chan));
    feed(p, RADIO_MUX_AEV_REOPENED);
}

static void run(mux_port_t *p, radio_mux_attach_act_t act)
{
    switch (act) {
    case RADIO_MUX_ACT_NONE:
        break;
    case RADIO_MUX_ACT_PROBE:
        start_probe(p);
        break;
    case RADIO_MUX_ACT_REOPEN:
        reopen(p);
        break;
    case RADIO_MUX_ACT_ATTACH:
        do_attach(p);
        break;
    case RADIO_MUX_ACT_DETACH:
        do_detach(p);
        break;
    case RADIO_MUX_ACT_REATTACH:
        do_detach(p);
        do_attach(p);
        break;
    case RADIO_MUX_ACT_GONE_CLOSE:
        do_detach(p);
        close_device(p);
        feed(p, RADIO_MUX_AEV_CLOSED);
        break;
    case RADIO_MUX_ACT_CLOSE:
        close_device(p);
        feed(p, RADIO_MUX_AEV_CLOSED);
        break;
    case RADIO_MUX_ACT_DEFER:
        ESP_LOGI(TAG, "%04x:%04x waits for the old device to close", p->pending.vid, p->pending.pid);
        break;
    }
}

// Owner: one event into the transport's attach machine; the action runs (and may feed the next event).
static void feed(mux_port_t *p, radio_mux_attach_ev_t ev)
{
    const radio_mux_attach_act_t act = radio_mux_attach_next(&p->att, ev, want_attach(p));

    run(p, act);
    refresh(p);
}

/* ---- owner: messages ---- */

// An unsupported device: listed as unavailable while attached, never remembered.
static void list_unsupported(const mux_usb_dev_t *dev)
{
    char id[RADIO_MUX_ID_MAX];

    radio_mux_channel_id(dev->vid, dev->pid, dev->serial[0] ? dev->serial : NULL, id);
    struct radio_mux_chan *ch = find_chan(id);

    if (!ch) {
        ch = evict_slot();
        if (!ch) {
            return;
        }
        lock();
        chan_defaults(ch, id, RADIO_MUX_KIND_USB);
        unlock();
    }
    lock();
    fill_usb(ch, dev);
    ch->present = true;
    ch->supported = false;
    ch->eff_mode = RADIO_MUX_MODE_UNAVAILABLE;
    ch->state = RADIO_MUX_STATE_IDLE;
    snprintf(ch->reason, sizeof(ch->reason), "no driver for %04x:%04x", dev->vid, dev->pid);
    unlock();
    ESP_LOGI(TAG, "%s: %s", ch->id, ch->reason);
}

// One port: a device that is listed but not open (unsupported, or it failed to open) is gone when another appears.
static void drop_unopened(mux_port_t *p, const mux_usb_dev_t *dev)
{
    for (size_t i = 1; i < RADIO_MUX_CHANNELS_MAX; i++) {
        struct radio_mux_chan *ch = &s_chans[i];

        if (ch->used && ch->present && !ch->supported && ch->usb.addr != dev->addr) {
            lock();
            ch->present = false;
            ch->used = ch->remembered; // an unsupported device leaves the list
            unlock();
        }
    }
    if (p->att.state == RADIO_MUX_AS_IDLE && !usb_transport_is_open() && p->dev.addr != dev->addr) {
        feed(p, RADIO_MUX_AEV_UNPLUG);
    }
}

static void on_new_dev(const mux_usb_dev_t *dev)
{
    mux_port_t *p = &s_usb_port;

    ESP_LOGI(TAG, "USB device %04x:%04x at %u, driver %s", dev->vid, dev->pid, dev->addr,
             dev->driver == RADIO_MUX_DRV_NONE ? "none (unsupported)" : radio_mux_driver_name(dev->driver));
    drop_unopened(p, dev);
    if (dev->driver == RADIO_MUX_DRV_NONE) {
        list_unsupported(dev);
        p->presence_check_us = esp_timer_get_time();
        return;
    }
    if (p->att.state != RADIO_MUX_AS_ABSENT && p->att.state != RADIO_MUX_AS_CLOSING) {
        ESP_LOGW(TAG, "%04x:%04x ignored: one USB radio at a time", dev->vid, dev->pid);
        return;
    }
    p->pending = *dev;
    feed(p, RADIO_MUX_AEV_PLUG);
}

static void on_gone(const void *hdl)
{
    if (usb_transport_is(hdl)) {
        feed(&s_usb_port, RADIO_MUX_AEV_UNPLUG);
    }
}

// Devices that are not open send no disconnect event: look them up now and then.
static void check_presence(mux_port_t *p)
{
    const int64_t now = esp_timer_get_time();

    if (now - p->presence_check_us < PRESENCE_POLL_US) {
        return;
    }
    p->presence_check_us = now;
    for (size_t i = 1; i < RADIO_MUX_CHANNELS_MAX; i++) {
        struct radio_mux_chan *ch = &s_chans[i];

        if (ch->used && ch->present && !ch->supported && !usb_transport_dev_attached(ch->usb.addr)) {
            ESP_LOGI(TAG, "%s gone", ch->id);
            lock();
            ch->present = false;
            ch->used = ch->remembered;
            unlock();
        }
    }
    if (p->att.state == RADIO_MUX_AS_IDLE && !usb_transport_is_open() && !usb_transport_dev_attached(p->dev.addr)) {
        feed(p, RADIO_MUX_AEV_UNPLUG);
    }
}

static radio_mux_verdict_t check_req(const struct radio_mux_chan *ch, const radio_mux_set_req_t *req,
                                     radio_mux_proxy_settings_t *proxy)
{
    *proxy = ch->proxy;
    if (req->has_baud && !radio_mux_baud_ok(req->baud)) {
        return RADIO_MUX_REFUSE_BAD_VALUE;
    }
    if (req->has_proxy) {
        if (req->proxy_fields & RADIO_MUX_PROXY_F_DIR) {
            proxy->dir = req->proxy.dir;
        }
        if (req->proxy_fields & RADIO_MUX_PROXY_F_ADAPTER) {
            proxy->adapter = req->proxy.adapter;
        }
        if (req->proxy_fields & RADIO_MUX_PROXY_F_PORT) {
            proxy->port = req->proxy.port;
        }
        if (req->proxy_fields & RADIO_MUX_PROXY_F_HOST) {
            memcpy(proxy->host, req->proxy.host, sizeof(proxy->host));
        }
        if (req->proxy_fields & RADIO_MUX_PROXY_F_FRAMING) {
            proxy->framing = req->proxy.framing;
        }
        if (!radio_mux_proxy_ok(proxy)) {
            return RADIO_MUX_REFUSE_BAD_VALUE;
        }
    }
    return RADIO_MUX_APPLY;
}

static radio_mux_verdict_t check_mode(struct radio_mux_chan *ch, radio_mux_mode_t want, char *from, size_t cap)
{
    radio_mux_chan_view_t views[RADIO_MUX_CHANNELS_MAX];
    size_t n = 0, target = 0, move_from = 0;

    for (size_t i = 0; i < RADIO_MUX_CHANNELS_MAX; i++) {
        if (s_chans[i].used) {
            if (&s_chans[i] == ch) {
                target = n;
            }
            views[n++] = view_of(&s_chans[i]);
        }
    }
    const radio_mux_verdict_t v = radio_mux_check_set_mode(&s_rules, views, n, target, want, &move_from);

    if (v == RADIO_MUX_MOVE) {
        snprintf(from, cap, "%s", views[move_from].id);
    }
    return v;
}

static void handle_set(mux_call_t *call)
{
    struct radio_mux_chan *ch = find_chan(call->id);
    const radio_mux_set_req_t *req = &call->req;
    radio_mux_proxy_settings_t proxy;

    call->from[0] = '\0';
    if (!ch) {
        call->verdict = RADIO_MUX_REFUSE_UNKNOWN_CHANNEL;
        return;
    }
    radio_mux_verdict_t v = check_req(ch, req, &proxy);

    if (v == RADIO_MUX_APPLY && req->has_mode) {
        v = check_mode(ch, req->mode, call->from, sizeof(call->from));
        if (v == RADIO_MUX_MOVE && !call->have_mover) {
            v = RADIO_MUX_REFUSE_NOT_AVAILABLE; // nothing runs a move yet (Task 7)
        }
    }
    call->verdict = v;
    if (v != RADIO_MUX_APPLY && v != RADIO_MUX_APPLY_REBOOT) {
        return; // refused, or a MOVE: the caller runs the mover; nothing stored here
    }
    const struct radio_mux_chan old = *ch;
    const bool mode_changed = req->has_mode && req->mode != ch->mode;
    const bool proxy_changed = req->has_proxy && memcmp(&proxy, &ch->proxy, sizeof(proxy)) != 0;
    const bool line_changed = (req->has_baud && req->baud != eff_baud(ch)) || (req->has_flow && req->flow != ch->flow);

    lock();
    if (req->has_mode) {
        ch->mode = req->mode;
    }
    if (req->has_baud) {
        ch->baud = req->baud;
    }
    if (req->has_flow) {
        ch->flow = req->flow;
    }
    ch->proxy = proxy;
    unlock();
    if (store_chan(ch) != ESP_OK) {
        lock();
        *ch = old;
        unlock();
        call->verdict = RADIO_MUX_REFUSE_FAILED;
        return;
    }
    mux_port_t *p = ch->port;

    if (v == RADIO_MUX_APPLY_REBOOT || p->chan != ch || p->kind != RADIO_MUX_KIND_USB) {
        return; // stored: applies after the reboot, or when the channel returns (the C6 has no transport yet)
    }
    const bool settled = p->att.state == RADIO_MUX_AS_IDLE || p->att.state == RADIO_MUX_AS_ATTACHED;

    if (line_changed && settled && usb_transport_is_open()) {
        line_dev(p, eff_baud(ch)); // a failure closes the device: the machine detaches below
    }
    // A PUT of the mode retries a fault (spec: Board mode, faults and status): a device that failed to open gets
    // another open, a consumer whose attach failed another attach.
    if (req->has_mode && p->att.state == RADIO_MUX_AS_IDLE && !usb_transport_is_open()) {
        open_dev(p, eff_baud(ch));
    }
    if (mode_changed || (proxy_changed && ch->mode == RADIO_MUX_MODE_RCP_PROXY) ||
            (req->has_mode && p->att.state == RADIO_MUX_AS_IDLE) ||
            (p->att.state == RADIO_MUX_AS_ATTACHED && !usb_transport_is_open())) {
        if (p->att.state == RADIO_MUX_AS_IDLE && usb_transport_is_open()) {
            p->fault[0] = '\0'; // a failed attach is tried again
        }
        feed(p, RADIO_MUX_AEV_MODE_SET);
    }
    refresh(p);
}

static void handle_forget(mux_call_t *call)
{
    struct radio_mux_chan *ch = find_chan(call->id);

    if (!ch) {
        call->err = ESP_ERR_NOT_FOUND;
        return;
    }
    if (ch->kind == RADIO_MUX_KIND_C6 || ch->present || ch->port->chan == ch) {
        call->err = ESP_ERR_INVALID_STATE;
        return;
    }
    call->err = erase_chan(ch);
    if (call->err == ESP_OK) {
        lock();
        ch->used = false;
        unlock();
        ESP_LOGI(TAG, "%s forgotten", call->id);
    }
}

// A dongle reset: close and reopen over USB, then RESET. GONE (detach) when it does not reopen.
static void handle_reset(struct radio_mux_chan *ch)
{
    mux_port_t *p = ch->port;

    if (p->chan != ch || p->att.state != RADIO_MUX_AS_ATTACHED || p->kind != RADIO_MUX_KIND_USB) {
        return; // the C6 reset is Task 6's; a channel that is not attached has nobody to tell
    }
    atomic_store(&p->tx_ok, false);
    set_rx(p, MUX_RX_DISCARD);
    usb_transport_close(p);
    if (!open_dev(p, eff_baud(ch))) {
        feed(p, RADIO_MUX_AEV_MODE_SET); // not open: the machine detaches the consumer (GONE)
        return;
    }
    taskENTER_CRITICAL(&p->lock);
    p->ctr.resets++;
    taskEXIT_CRITICAL(&p->lock);
    set_rx(p, p->tx_frames ? MUX_RX_FRAMES : MUX_RX_RAW);
    atomic_store(&p->tx_ok, true);
    post_ctl(p, RADIO_MUX_EV_RESET);
    ESP_LOGI(TAG, "%s: reopened (reset)", ch->id);
}

static void handle_note_fw(struct radio_mux_chan *ch, const radio_mux_fw_record_t *fw)
{
    lock();
    const uint32_t baud = ch->fw.rec.baud;

    ch->fw.rec = *fw;
    ch->fw.rec.version[sizeof(ch->fw.rec.version) - 1] = '\0';
    ch->fw.rec.chip[sizeof(ch->fw.rec.chip) - 1] = '\0';
    if (ch->fw.rec.baud == 0) {
        ch->fw.rec.baud = baud ? baud : eff_baud(ch);
    }
    ch->fw.source = RADIO_MUX_FW_SRC_PASSIVE;
    ch->fw.at = now_epoch();
    unlock();
    if (ch->kind == RADIO_MUX_KIND_C6 || ch->remembered) {
        store_chan(ch);
    }
}

static void call_put(mux_call_t *call)
{
    if (atomic_fetch_sub(&call->refs, 1) == 1) {
        vSemaphoreDelete(call->done);
        free(call);
    }
}

static void handle(const mux_msg_t *m)
{
    switch (m->type) {
    case MUX_MSG_NEW_DEV:
        on_new_dev(&m->dev);
        break;
    case MUX_MSG_GONE:
        on_gone(m->hdl);
        break;
    case MUX_MSG_SET:
        handle_set(m->call);
        xSemaphoreGive(m->call->done);
        call_put(m->call);
        break;
    case MUX_MSG_FORGET:
        handle_forget(m->call);
        xSemaphoreGive(m->call->done);
        call_put(m->call);
        break;
    case MUX_MSG_RESET:
        handle_reset(m->ch);
        break;
    case MUX_MSG_NOTE_FW:
        handle_note_fw(m->note.ch, &m->note.fw);
        break;
    }
}

static void owner_task(void *arg)
{
    mux_msg_t m;
    const esp_err_t err = usb_transport_start();

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "USB host start: %s, no USB radios", esp_err_to_name(err));
    }
    // R13 at start: every present channel whose stored mode has a consumer. Before the C6 transport (Task 6) no
    // channel is present yet; USB devices attached at boot arrive through new_dev_cb like any plug-in.
    for (;;) {
        const bool probing = s_usb_port.att.state == RADIO_MUX_AS_PROBING;

        if (xQueueReceive(s_queue, &m, WAIT_TICKS(probing ? DETECT_POLL_MS : IDLE_POLL_MS)) == pdTRUE) {
            handle(&m);
            while (xQueueReceive(s_queue, &m, 0) == pdTRUE) {
                handle(&m);
            }
        }
        if (s_usb_port.att.state == RADIO_MUX_AS_PROBING) {
            detect_step(&s_usb_port);
        }
        check_presence(&s_usb_port);
    }
}

/* ---- start ---- */

static esp_err_t port_init(mux_port_t *p, radio_mux_kind_t kind, bool transport)
{
    memset(p, 0, sizeof(*p));
    p->kind = kind;
    p->att.state = RADIO_MUX_AS_ABSENT;
    portMUX_INITIALIZE(&p->lock);
    p->efd = -1;
    atomic_store(&p->tx_ok, false);
    p->evq = xQueueCreate(EVQ_LEN, sizeof(radio_mux_ev_t));
    p->rx_lock = xSemaphoreCreateMutex();
    p->tx_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(p->evq && p->rx_lock && p->tx_lock, ESP_ERR_NO_MEM, TAG, "port queue and locks");
    p->efd = eventfd(0, 0);
    ESP_RETURN_ON_FALSE(p->efd >= 0, ESP_ERR_NO_MEM, TAG, "eventfd (is the eventfd VFS registered?)");
    if (!transport) {
        return ESP_OK;
    }
    // Slots are DMA-capable and cache-line aligned (the C6 transport DMAs into them; USB copies in the callback).
    uint8_t *slots = heap_caps_aligned_alloc(MUX_CACHE_LINE, (size_t)MUX_SLOT_SIZE * RADIO_MUX_POOL_SLOTS,
                                             MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);

    p->discard = malloc(MUX_SLOT_SIZE);
    p->tx_cap = RADIO_MUX_HDLC_ENCODED_MAX(RADIO_MUX_FRAME_MAX);
    p->tx_buf = malloc(p->tx_cap);
    p->ring = xRingbufferCreate(RX_RING_SIZE, RINGBUF_TYPE_BYTEBUF);
    ESP_RETURN_ON_FALSE(slots && p->discard && p->tx_buf && p->ring, ESP_ERR_NO_MEM, TAG, "port buffers");
    radio_mux_pool_init(&p->pool, slots, MUX_SLOT_SIZE, RADIO_MUX_POOL_SLOTS);
    return ESP_OK;
}

esp_err_t radio_mux_register_consumer(const radio_mux_consumer_t *c)
{
    ESP_RETURN_ON_FALSE(c && c->attach && c->detach, ESP_ERR_INVALID_ARG, TAG, "consumer needs attach and detach");
    ESP_RETURN_ON_FALSE(c->mode == RADIO_MUX_MODE_OTBR || c->mode == RADIO_MUX_MODE_ZIGGURAT ||
                        c->mode == RADIO_MUX_MODE_RCP_PROXY, ESP_ERR_INVALID_ARG, TAG, "no consumer for mode %d",
                        (int)c->mode);
    ESP_RETURN_ON_FALSE(!atomic_load(&s_started), ESP_ERR_INVALID_STATE, TAG, "register before radio_mux_start");
    ESP_RETURN_ON_FALSE(!find_consumer(c->mode), ESP_ERR_INVALID_STATE, TAG, "%s already has a consumer",
                        radio_mux_mode_name(c->mode));
    ESP_RETURN_ON_FALSE(s_n_consumers < RADIO_MUX_RULES_MAX, ESP_ERR_NO_MEM, TAG, "too many consumers");
    s_consumers[s_n_consumers++] = *c;
    s_rules.rules[s_rules.n++] = (radio_mux_rule_t) {
        .mode = c->mode, .needs = c->needs, .limit = c->limit, .reboot_on_change = c->reboot_on_change,
    };
    ESP_LOGI(TAG, "%s consumer registered", radio_mux_mode_name(c->mode));
    return ESP_OK;
}

esp_err_t radio_mux_start(void)
{
    ESP_RETURN_ON_FALSE(!atomic_load(&s_started), ESP_ERR_INVALID_STATE, TAG, "already started");
    s_lock = xSemaphoreCreateMutex();
    s_queue = xQueueCreate(QUEUE_LEN, sizeof(mux_msg_t));
    ESP_RETURN_ON_FALSE(s_lock && s_queue, ESP_ERR_NO_MEM, TAG, "queue and lock");
    ESP_RETURN_ON_ERROR(port_init(&s_c6_port, RADIO_MUX_KIND_C6, false), TAG, "c6 port");
    ESP_RETURN_ON_ERROR(port_init(&s_usb_port, RADIO_MUX_KIND_USB, true), TAG, "USB port");
    load_chans();
    atomic_store(&s_started, true);
    if (xTaskCreatePinnedToCore(owner_task, "radio_mux", OWNER_STACK, NULL, OWNER_PRIORITY, NULL, MGMT_CORE) !=
            pdPASS) {
        atomic_store(&s_started, false);
        return ESP_ERR_NO_MEM;
    }
    size_t n = 0;

    for (size_t i = 0; i < RADIO_MUX_CHANNELS_MAX; i++) {
        n += s_chans[i].used;
    }
    ESP_LOGI(TAG, "started: %u channels known, %u consumers", (unsigned)n, (unsigned)s_n_consumers);
    return ESP_OK;
}

bool mux_post(const mux_msg_t *m)
{
    return s_queue && xQueueSend(s_queue, m, 0) == pdTRUE;
}

/* ---- consumer side ---- */

esp_err_t radio_mux_bind_queue(radio_mux_chan_t *ch, QueueHandle_t q)
{
    ESP_RETURN_ON_FALSE(ch && atomic_load(&s_started), ESP_ERR_INVALID_STATE, TAG, "not started");
    mux_port_t *p = ch->port;
    radio_mux_ev_t ev;

    xSemaphoreTake(p->rx_lock, portMAX_DELAY);
    p->bound = q;
    // What arrived between the start of the receive path and this call moves over, in order.
    while (q && xQueueReceive(p->evq, &ev, 0) == pdTRUE) {
        if (xQueueSend(q, &ev, 0) != pdTRUE && ev.type == RADIO_MUX_EV_RX_FRAME) {
            slot_put(p, ev.slot);
            taskENTER_CRITICAL(&p->lock);
            p->ctr.drops++;
            taskEXIT_CRITICAL(&p->lock);
        }
    }
    xSemaphoreGive(p->rx_lock);
    return ESP_OK;
}

int radio_mux_eventfd(radio_mux_chan_t *ch)
{
    return ch && atomic_load(&s_started) ? ch->port->efd : -1;
}

bool radio_mux_poll(radio_mux_chan_t *ch, radio_mux_ev_t *ev)
{
    if (!ch || !atomic_load(&s_started)) {
        return false;
    }
    mux_port_t *p = ch->port;

    if (xQueueReceive(p->evq, ev, 0) == pdTRUE) {
        return true;
    }
    rearm(p);
    return false;
}

void radio_mux_release(radio_mux_chan_t *ch, uint8_t *slot)
{
    if (ch && slot) {
        slot_put(ch->port, slot);
    }
}

esp_err_t radio_mux_tx(radio_mux_chan_t *ch, const uint8_t *data, size_t n)
{
    if (!ch || !atomic_load(&s_started)) {
        return ESP_ERR_INVALID_STATE;
    }
    mux_port_t *p = ch->port;

    if (!atomic_load(&p->tx_ok) || p->chan != ch) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!data || n == 0 || (p->tx_frames && n > RADIO_MUX_FRAME_MAX)) {
        return n == 0 ? ESP_ERR_INVALID_ARG : ESP_ERR_INVALID_SIZE;
    }
    if (p->kind != RADIO_MUX_KIND_USB) {
        return ESP_ERR_INVALID_STATE; // the C6 transport is Task 6's
    }
    esp_err_t err;

    xSemaphoreTake(p->tx_lock, portMAX_DELAY);
    if (!atomic_load(&p->tx_ok)) {
        err = ESP_ERR_INVALID_STATE;
    } else if (p->tx_frames) {
        const size_t len = radio_mux_hdlc_encode(data, n, p->tx_buf, p->tx_cap);

        err = len ? usb_transport_write(p->tx_buf, len) : ESP_ERR_INVALID_SIZE;
    } else {
        err = usb_transport_write(data, n);
    }
    xSemaphoreGive(p->tx_lock);
    if (err == ESP_OK) {
        taskENTER_CRITICAL(&p->lock);
        p->ctr.frames_out += p->tx_frames ? 1 : 0;
        p->ctr.bytes_out += n;
        taskEXIT_CRITICAL(&p->lock);
    }
    return err;
}

esp_err_t radio_mux_reset(radio_mux_chan_t *ch)
{
    ESP_RETURN_ON_FALSE(ch && atomic_load(&s_started), ESP_ERR_INVALID_STATE, TAG, "not started");
    const mux_msg_t m = {.type = MUX_MSG_RESET, .ch = ch};

    return mux_post(&m) ? ESP_OK : ESP_ERR_NO_MEM;
}

size_t radio_mux_read_raw(radio_mux_chan_t *ch, uint8_t *out, size_t cap)
{
    if (!ch || !atomic_load(&s_started) || !ch->port->ring) {
        return 0;
    }
    mux_port_t *p = ch->port;
    const size_t n = ring_take(p, out, cap);

    if (n == 0) {
        rearm(p);
    }
    return n;
}

const char *radio_mux_chan_id(const radio_mux_chan_t *ch)
{
    return ch ? ch->id : NULL;
}

void radio_mux_get_proxy(const radio_mux_chan_t *ch, radio_mux_proxy_settings_t *out)
{
    if (!ch || !s_lock) {
        radio_mux_proxy_defaults(out);
        return;
    }
    lock();
    *out = ch->proxy;
    unlock();
}

void radio_mux_note_firmware(radio_mux_chan_t *ch, const radio_mux_fw_record_t *fw)
{
    if (!ch || !fw) {
        return;
    }
    mux_msg_t m = {.type = MUX_MSG_NOTE_FW};

    m.note.ch = ch;
    m.note.fw = *fw;
    if (!mux_post(&m)) {
        ESP_LOGW(TAG, "%s: firmware note lost: queue full", ch->id);
    }
}

/* ---- API side ---- */

static void fill_info(const struct radio_mux_chan *ch, radio_mux_chan_info_t *o)
{
    memset(o, 0, sizeof(*o));
    memcpy(o->id, ch->id, sizeof(o->id));
    o->kind = ch->kind;
    memcpy(o->label, ch->label, sizeof(o->label));
    o->present = ch->present;
    o->supported = ch->supported;
    o->mode = ch->eff_mode;
    o->stored_mode = ch->mode;
    o->state = ch->state;
    memcpy(o->reason, ch->reason, sizeof(o->reason));
    o->line.baud = eff_baud(ch);
    o->line.flow_control = ch->flow;
    if (ch->kind == RADIO_MUX_KIND_USB) {
        o->usb.vid = ch->usb.vid;
        o->usb.pid = ch->usb.pid;
        memcpy(o->usb.serial, ch->usb.serial, sizeof(o->usb.serial));
        o->usb.driver = ch->usb.driver;
        o->caps.flow_control = ch->usb.driver == RADIO_MUX_DRV_CP210X;
    } else {
        o->caps.firmware_update = true;
        o->caps.hw_reset = true;
    }
    o->proxy = ch->proxy;
    o->firmware = ch->fw;
    mux_port_t *p = ch->port;

    if (p->chan == ch) {
        taskENTER_CRITICAL(&p->lock);
        o->counters = p->ctr;
        taskEXIT_CRITICAL(&p->lock);
    } else {
        o->counters = ch->ctr;
    }
}

void radio_mux_get_snapshot(radio_mux_snapshot_t *out)
{
    out->n = 0;
    if (!atomic_load(&s_started)) {
        return;
    }
    lock();
    for (size_t i = 0; i < RADIO_MUX_CHANNELS_MAX; i++) {
        if (s_chans[i].used) {
            fill_info(&s_chans[i], &out->chans[out->n++]);
        }
    }
    unlock();
}

// Posts a call to the owner and waits for it. ESP_ERR_TIMEOUT when the owner did not answer in time: the record is
// then freed by the owner when it gets to it.
static esp_err_t call_owner(mux_msg_type_t type, mux_call_t *call)
{
    const mux_msg_t m = {.type = type, .call = call};

    if (xQueueSend(s_queue, &m, pdMS_TO_TICKS(CALL_WAIT_MS)) != pdTRUE) {
        call_put(call); // the owner's reference
        return ESP_ERR_TIMEOUT;
    }
    if (xSemaphoreTake(call->done, pdMS_TO_TICKS(CALL_WAIT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "the owner task did not answer in %d ms", CALL_WAIT_MS);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static mux_call_t *call_new(const char *id)
{
    mux_call_t *call = calloc(1, sizeof(*call));

    if (!call) {
        return NULL;
    }
    call->done = xSemaphoreCreateBinary();
    if (!call->done) {
        free(call);
        return NULL;
    }
    atomic_store(&call->refs, 2);
    snprintf(call->id, sizeof(call->id), "%s", id);
    return call;
}

radio_mux_verdict_t radio_mux_set(const char *id, const radio_mux_set_req_t *req, char *moved_from, size_t cap)
{
    if (!id || !req || !atomic_load(&s_started)) {
        return RADIO_MUX_REFUSE_FAILED;
    }
    esp_err_t (*mover)(const char *, const char *) = atomic_load(&s_mover);
    mux_call_t *call = call_new(id);

    if (!call) {
        return RADIO_MUX_REFUSE_FAILED;
    }
    call->req = *req;
    call->req.kick = false; // never stored; device_api kicks through rcp_proxy (R6)
    call->have_mover = mover != NULL;
    if (call_owner(MUX_MSG_SET, call) != ESP_OK) {
        call_put(call);
        return RADIO_MUX_REFUSE_FAILED;
    }
    radio_mux_verdict_t v = call->verdict;
    char from[RADIO_MUX_ID_MAX];

    snprintf(from, sizeof(from), "%s", call->from);
    call_put(call);
    if (v != RADIO_MUX_MOVE) {
        return v;
    }
    // The move runs on the caller's task, never the owner's: its probe goes through the mux.
    const esp_err_t err = mover(from, id);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "moving otbr from %s to %s: %s", from, id, esp_err_to_name(err));
        return RADIO_MUX_REFUSE_NOT_AVAILABLE;
    }
    if (moved_from && cap > 0) {
        snprintf(moved_from, cap, "%s", from);
    }
    // The line and rcp_proxy parts of the request apply like any other change; the move stores the modes.
    radio_mux_set_req_t rest = *req;

    rest.has_mode = false;
    if (rest.has_baud || rest.has_flow || rest.has_proxy) {
        const radio_mux_verdict_t r = radio_mux_set(id, &rest, NULL, 0);

        if (r != RADIO_MUX_APPLY) {
            ESP_LOGW(TAG, "%s: line settings after the move: verdict %d", id, (int)r);
        }
    }
    return RADIO_MUX_MOVE;
}

esp_err_t radio_mux_forget(const char *id)
{
    ESP_RETURN_ON_FALSE(id && atomic_load(&s_started), ESP_ERR_INVALID_STATE, TAG, "not started");
    mux_call_t *call = call_new(id);

    ESP_RETURN_ON_FALSE(call, ESP_ERR_NO_MEM, TAG, "call");
    esp_err_t err = call_owner(MUX_MSG_FORGET, call);

    if (err == ESP_OK) {
        err = call->err;
    }
    call_put(call);
    return err;
}

const char *radio_mux_otbr_channel(void)
{
    const char *id = NULL;

    if (!atomic_load(&s_started)) {
        return NULL;
    }
    lock();
    for (size_t i = 0; i < RADIO_MUX_CHANNELS_MAX && !id; i++) {
        if (s_chans[i].used && s_chans[i].mode == RADIO_MUX_MODE_OTBR) {
            id = s_chans[i].id;
        }
    }
    unlock();
    return id;
}

void radio_mux_set_otbr_mover(esp_err_t (*move)(const char *from, const char *to))
{
    atomic_store(&s_mover, move);
}
