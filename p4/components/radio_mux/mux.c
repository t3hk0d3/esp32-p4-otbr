// The radio channel mux at runtime: the owner task (plug-in, detection, attach, unplug, settings) and the start
// (spec section 3). The registry's NVS is in mux_nvs.c, the delivery of frames and events in mux_deliver.c, the
// consumer and API calls in mux_api.c.
//
// Only the owner task changes a channel or a transport's attach state, opens or closes a device and calls a consumer's
// attach or detach. The transports post messages to it and deliver received frames themselves without waiting. The
// API calls post a message and wait for the owner's answer.
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "board_cores.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_eventfd.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "radio_mux_priv.h"

static const char *TAG = "radio_mux";

#define OWNER_PRIORITY 6
#define OWNER_STACK 6144
#define QUEUE_LEN 12
#define DETECT_POLL_MS 10
#define IDLE_POLL_MS 1000
#define PRESENCE_POLL_US 1000000 // how often a device that is not open is checked for still being there
#define DETECT_CHUNK 256
#define EVQ_LEN (RADIO_MUX_POOL_SLOTS + 4) // frames hold a slot each, so control events always fit
#define DEFAULT_BAUD 460800
#define EPOCH_VALID 1577836800 // 2020-01-01: an earlier clock was never set

// At least one tick: at CONFIG_FREERTOS_HZ 100, pdMS_TO_TICKS(10) is 1 and smaller waits would not block.
#define WAIT_TICKS(ms) (pdMS_TO_TICKS(ms) > 0 ? pdMS_TO_TICKS(ms) : 1)

mux_t g_mux;

static void feed(mux_port_t *p, radio_mux_attach_ev_t ev);

/* ---- small helpers ---- */

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

int64_t mux_now_epoch(void)
{
    const time_t t = time(NULL);

    return t >= EPOCH_VALID ? (int64_t)t : 0;
}

void mux_lock(void)
{
    xSemaphoreTake(g_mux.lock, portMAX_DELAY);
}

void mux_unlock(void)
{
    xSemaphoreGive(g_mux.lock);
}

uint32_t mux_eff_baud(const struct radio_mux_chan *ch)
{
    return ch->baud ? ch->baud : DEFAULT_BAUD;
}

const radio_mux_consumer_t *mux_find_consumer(radio_mux_mode_t mode)
{
    for (size_t i = 0; i < g_mux.n_consumers; i++) {
        if (g_mux.consumers[i].mode == mode) {
            return &g_mux.consumers[i];
        }
    }
    return NULL;
}

static struct radio_mux_chan *find_chan(const char *id)
{
    for (size_t i = 0; i < RADIO_MUX_CHANNELS_MAX; i++) {
        if (g_mux.chans[i].used && strcmp(g_mux.chans[i].id, id) == 0) {
            return &g_mux.chans[i];
        }
    }
    return NULL;
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

    return radio_mux_attach_wanted(&g_mux.rules, &v, mux_find_consumer(ch->mode) != NULL);
}

// Owner: what the API shows for the channel on port p (mode, state, reason), from the attach state.
static void refresh(mux_port_t *p)
{
    struct radio_mux_chan *ch = p->chan;

    if (!ch) {
        return;
    }
    mux_lock();
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
        const radio_mux_mode_t m = radio_mux_after_detect(&g_mux.rules, ch->mode, ch->fw.rec.type, why, sizeof(why));

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
    mux_unlock();
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
    const radio_mux_consumer_t *c = mux_find_consumer(ch->mode);

    if (!c) { // want_attach said yes, so this does not happen; keep the machine consistent anyway
        snprintf(p->fault, sizeof(p->fault), "no %s consumer", radio_mux_mode_name(ch->mode));
        feed(p, RADIO_MUX_AEV_ATTACH_FAILED);
        return;
    }
    mux_drain_evq(p);
    xSemaphoreTake(p->rx_lock, portMAX_DELAY);
    p->bound = NULL;
    xSemaphoreGive(p->rx_lock);
    atomic_store(&p->evq_chan, ch); // from here on the queue, ring and TX are this channel's consumer's
    p->consumer = c;
    p->tx_frames = radio_mux_port_frames(p->kind == RADIO_MUX_KIND_C6, c->frames); // the C6 ignores c->frames
    mux_set_rx(p, p->tx_frames ? MUX_RX_FRAMES : MUX_RX_RAW);
    atomic_store(&p->tx_ok, true);
    const esp_err_t err = c->attach((radio_mux_chan_t *)ch, c->ctx);

    if (err != ESP_OK) {
        atomic_store(&p->tx_ok, false);
        mux_set_rx(p, MUX_RX_DISCARD);
        xSemaphoreTake(p->rx_lock, portMAX_DELAY);
        p->bound = NULL;
        xSemaphoreGive(p->rx_lock);
        mux_drain_evq(p);
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
    mux_set_rx(p, MUX_RX_DISCARD);
    if (!c) {
        return;
    }
    mux_post_ctl(p, RADIO_MUX_EV_GONE);
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
    mux_set_rx(p, MUX_RX_DISCARD);
    if (p->kind == RADIO_MUX_KIND_USB) {
        usb_transport_close(p);
    }
    if (!ch) {
        return;
    }
    mux_lock();
    taskENTER_CRITICAL(&p->lock);
    ch->ctr = p->ctr;
    taskEXIT_CRITICAL(&p->lock);
    ch->present = false;
    ch->last_seen = mux_now_epoch();
    ch->eff_mode = RADIO_MUX_MODE_UNAVAILABLE;
    ch->state = RADIO_MUX_STATE_IDLE;
    snprintf(ch->reason, sizeof(ch->reason), "not detected");
    p->chan = NULL;
    mux_unlock();
    if (ch->remembered) {
        mux_store_chan(ch);
    }
    ESP_LOGI(TAG, "%s: unplugged", ch->id);
}

static struct radio_mux_chan *evict_slot(void)
{
    struct radio_mux_chan *oldest = NULL;

    for (size_t i = 1; i < RADIO_MUX_CHANNELS_MAX; i++) {
        struct radio_mux_chan *ch = &g_mux.chans[i];

        if (!ch->used) {
            return ch;
        }
        if (!ch->present && ch->port->chan != ch && (!oldest || ch->last_seen < oldest->last_seen)) {
            oldest = ch;
        }
    }
    if (oldest) {
        ESP_LOGW(TAG, "%s forgotten: room for a new channel", oldest->id);
        mux_erase_chan(oldest);
        mux_lock();
        oldest->used = false;
        mux_unlock();
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
    mux_lock();
    mux_chan_defaults(ch, id, RADIO_MUX_KIND_USB);
    ch->first_seen = mux_now_epoch();
    mux_unlock();
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
    mux_lock();
    p->chan = ch;
    fill_usb(ch, &p->dev);
    ch->present = true;
    ch->supported = true;
    ch->last_seen = mux_now_epoch();
    mux_unlock();
    taskENTER_CRITICAL(&p->lock);
    p->ctr = ch->ctr;
    taskEXIT_CRITICAL(&p->lock);
    mux_store_chan(ch); // a supported dongle is remembered from its first plug-in
    refresh(p);
    ESP_LOGI(TAG, "%s (%s): detecting the firmware", ch->id, ch->label);
    radio_mux_detect_start(&p->det, now_ms());
    radio_mux_detect_next(&p->det, now_ms(), NULL, 0, &p->det_baud, &tx, &tx_n, &p->det_wait_ms, &unused);
    if (!open_dev(p, p->det_baud)) {
        feed(p, RADIO_MUX_AEV_DETECT_DONE); // no detection without a device: the reopen tries once more
        return;
    }
    mux_set_rx(p, MUX_RX_DETECT);
    write_dev(p, tx, tx_n);
}

static void detect_done(mux_port_t *p, const radio_mux_fw_record_t *rec)
{
    struct radio_mux_chan *ch = p->chan;

    mux_lock();
    ch->fw.rec = *rec;
    ch->fw.source = RADIO_MUX_FW_SRC_PROBE;
    ch->fw.at = mux_now_epoch();
    if (ch->baud == 0 && rec->baud != 0) { // the answering baud becomes the channel's when none was set
        ch->baud = rec->baud;
    }
    mux_unlock();
    mux_store_chan(ch);
    ESP_LOGI(TAG, "%s: firmware %s %s%s%s at %lu baud", ch->id, radio_mux_fw_name(rec->type), rec->version,
             rec->chip[0] ? ", " : "", rec->chip, (unsigned long)rec->baud);
    feed(p, RADIO_MUX_AEV_DETECT_DONE);
}

// Owner loop, while PROBING: hands the bytes received since the last call to the sequencer (R16).
static void detect_step(mux_port_t *p)
{
    uint8_t rx[DETECT_CHUNK];
    const size_t n = mux_ring_take(p, rx, sizeof(rx));
    const int64_t now = now_ms();
    uint32_t baud;
    const uint8_t *tx;
    size_t tx_n;
    radio_mux_fw_record_t rec;

    if (n == 0 && now < p->det_wait_ms) {
        return;
    }
    if (radio_mux_detect_next(&p->det, now, rx, n, &baud, &tx, &tx_n, &p->det_wait_ms, &rec) == RADIO_MUX_PROBE_DONE) {
        mux_set_rx(p, MUX_RX_DISCARD);
        detect_done(p, &rec);
        return;
    }
    // A new baud is new line coding on the open device: the "reopen at *baud" of the sequencer, without a close.
    if (baud != p->det_baud) {
        p->det_baud = baud;
        if (!line_dev(p, baud)) {
            mux_set_rx(p, MUX_RX_DISCARD);
            feed(p, RADIO_MUX_AEV_DETECT_DONE); // detection stopped; the old firmware record stays
            return;
        }
    }
    if (p->det.new_step) {
        mux_ring_flush(p); // a new probe step starts clean; a follow-up keeps what came
    }
    write_dev(p, tx, tx_n);
}

// REOPEN: the detection's bytes leave nothing behind; the device comes back at the channel's line settings.
static void reopen(mux_port_t *p)
{
    mux_set_rx(p, MUX_RX_DISCARD);
    usb_transport_close(p);
    open_dev(p, mux_eff_baud(p->chan));
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
        mux_lock();
        mux_chan_defaults(ch, id, RADIO_MUX_KIND_USB);
        mux_unlock();
    }
    mux_lock();
    fill_usb(ch, dev);
    ch->present = true;
    ch->supported = false;
    ch->eff_mode = RADIO_MUX_MODE_UNAVAILABLE;
    ch->state = RADIO_MUX_STATE_IDLE;
    snprintf(ch->reason, sizeof(ch->reason), "no driver for %04x:%04x", dev->vid, dev->pid);
    mux_unlock();
    ESP_LOGI(TAG, "%s: %s", ch->id, ch->reason);
}

// One port: a device that is listed but not open (unsupported, or it failed to open) is gone when another appears.
static void drop_unopened(mux_port_t *p, const mux_usb_dev_t *dev)
{
    for (size_t i = 1; i < RADIO_MUX_CHANNELS_MAX; i++) {
        struct radio_mux_chan *ch = &g_mux.chans[i];

        if (ch->used && ch->present && !ch->supported && ch->usb.addr != dev->addr) {
            mux_lock();
            ch->present = false;
            ch->used = ch->remembered; // an unsupported device leaves the list
            mux_unlock();
        }
    }
    if (p->att.state == RADIO_MUX_AS_IDLE && !usb_transport_is_open() && p->dev.addr != dev->addr) {
        feed(p, RADIO_MUX_AEV_UNPLUG);
    }
}

// An open device whose disconnect message never reached the owner (its queue was full): the transport flagged it,
// or its address left the bus. Unplugged here, so a lost message is never fatal; a late message then finds its
// handle closed and is ignored.
static void heal_lost_unplug(mux_port_t *p)
{
    if (!usb_transport_is_open()) {
        return;
    }
    if (atomic_load(&p->dev_gone) || !usb_transport_dev_attached(p->dev.addr)) {
        ESP_LOGW(TAG, "%s: disconnected (found by the presence check)", p->chan ? p->chan->id : "USB device");
        feed(p, RADIO_MUX_AEV_UNPLUG);
    }
}

static void on_new_dev(const mux_usb_dev_t *dev)
{
    mux_port_t *p = &g_mux.usb;

    ESP_LOGI(TAG, "USB device %04x:%04x at %u, driver %s", dev->vid, dev->pid, dev->addr,
             dev->driver == RADIO_MUX_DRV_NONE ? "none (unsupported)" : radio_mux_driver_name(dev->driver));
    heal_lost_unplug(p); // a new device while the old one still looks open: its unplug may have been lost
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
        feed(&g_mux.usb, RADIO_MUX_AEV_UNPLUG);
    }
}

// Devices that are not open send no disconnect event: look them up now and then. Open ones are checked too.
static void check_presence(mux_port_t *p)
{
    const int64_t now = esp_timer_get_time();

    if (now - p->presence_check_us < PRESENCE_POLL_US) {
        return;
    }
    p->presence_check_us = now;
    for (size_t i = 1; i < RADIO_MUX_CHANNELS_MAX; i++) {
        struct radio_mux_chan *ch = &g_mux.chans[i];

        if (ch->used && ch->present && !ch->supported && !usb_transport_dev_attached(ch->usb.addr)) {
            ESP_LOGI(TAG, "%s gone", ch->id);
            mux_lock();
            ch->present = false;
            ch->used = ch->remembered;
            mux_unlock();
        }
    }
    if (p->att.state == RADIO_MUX_AS_IDLE && !usb_transport_is_open() && !usb_transport_dev_attached(p->dev.addr)) {
        feed(p, RADIO_MUX_AEV_UNPLUG);
    }
    heal_lost_unplug(p);
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
        if (g_mux.chans[i].used) {
            if (&g_mux.chans[i] == ch) {
                target = n;
            }
            views[n++] = view_of(&g_mux.chans[i]);
        }
    }
    const radio_mux_verdict_t v = radio_mux_check_set_mode(&g_mux.rules, views, n, target, want, &move_from);

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
    const bool line_changed = (req->has_baud && req->baud != mux_eff_baud(ch)) || (req->has_flow && req->flow != ch->flow);

    mux_lock();
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
    mux_unlock();
    if (mux_store_chan(ch) != ESP_OK) {
        mux_lock();
        *ch = old;
        mux_unlock();
        call->verdict = RADIO_MUX_REFUSE_FAILED;
        return;
    }
    mux_port_t *p = ch->port;

    if (v == RADIO_MUX_APPLY_REBOOT || p->chan != ch || p->kind != RADIO_MUX_KIND_USB) {
        return; // stored: applies after the reboot, or when the channel returns (the C6 has no transport yet)
    }
    const bool settled = p->att.state == RADIO_MUX_AS_IDLE || p->att.state == RADIO_MUX_AS_ATTACHED;

    if (line_changed && settled && usb_transport_is_open()) {
        line_dev(p, mux_eff_baud(ch)); // a failure closes the device: the machine detaches below
    }
    // A PUT of the mode retries a fault (spec: Board mode, faults and status): a device that failed to open gets
    // another open, a consumer whose attach failed another attach.
    if (req->has_mode && p->att.state == RADIO_MUX_AS_IDLE && !usb_transport_is_open()) {
        open_dev(p, mux_eff_baud(ch));
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
    call->err = mux_erase_chan(ch);
    if (call->err == ESP_OK) {
        mux_lock();
        ch->used = false;
        mux_unlock();
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
    mux_set_rx(p, MUX_RX_DISCARD);
    usb_transport_close(p);
    if (!open_dev(p, mux_eff_baud(ch))) {
        feed(p, RADIO_MUX_AEV_MODE_SET); // not open: the machine detaches the consumer (GONE)
        return;
    }
    taskENTER_CRITICAL(&p->lock);
    p->ctr.resets++;
    taskEXIT_CRITICAL(&p->lock);
    mux_set_rx(p, p->tx_frames ? MUX_RX_FRAMES : MUX_RX_RAW);
    atomic_store(&p->tx_ok, true);
    mux_post_ctl(p, RADIO_MUX_EV_RESET);
    ESP_LOGI(TAG, "%s: reopened (reset)", ch->id);
}

static void handle_note_fw(struct radio_mux_chan *ch, const radio_mux_fw_record_t *fw)
{
    if (ch->port->chan != ch || ch->port->att.state != RADIO_MUX_AS_ATTACHED) {
        ESP_LOGW(TAG, "%s: firmware note from a consumer no longer attached: ignored", ch->id);
        return;
    }
    mux_lock();
    const uint32_t baud = ch->fw.rec.baud;

    ch->fw.rec = *fw;
    ch->fw.rec.version[sizeof(ch->fw.rec.version) - 1] = '\0';
    ch->fw.rec.chip[sizeof(ch->fw.rec.chip) - 1] = '\0';
    if (ch->fw.rec.baud == 0) {
        ch->fw.rec.baud = baud ? baud : mux_eff_baud(ch);
    }
    ch->fw.source = RADIO_MUX_FW_SRC_PASSIVE;
    ch->fw.at = mux_now_epoch();
    mux_unlock();
    if (ch->kind == RADIO_MUX_KIND_C6 || ch->remembered) {
        mux_store_chan(ch);
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
        mux_call_put(m->call);
        break;
    case MUX_MSG_FORGET:
        handle_forget(m->call);
        xSemaphoreGive(m->call->done);
        mux_call_put(m->call);
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
        const bool probing = g_mux.usb.att.state == RADIO_MUX_AS_PROBING;

        if (xQueueReceive(g_mux.queue, &m, WAIT_TICKS(probing ? DETECT_POLL_MS : IDLE_POLL_MS)) == pdTRUE) {
            handle(&m);
            while (xQueueReceive(g_mux.queue, &m, 0) == pdTRUE) {
                handle(&m);
            }
        }
        if (g_mux.usb.att.state == RADIO_MUX_AS_PROBING) {
            detect_step(&g_mux.usb);
        }
        check_presence(&g_mux.usb);
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
    p->ring = xRingbufferCreate(MUX_RX_RING_SIZE, RINGBUF_TYPE_BYTEBUF);
    ESP_RETURN_ON_FALSE(slots && p->discard && p->tx_buf && p->ring, ESP_ERR_NO_MEM, TAG, "port buffers");
    radio_mux_pool_init(&p->pool, slots, MUX_SLOT_SIZE, RADIO_MUX_POOL_SLOTS);
    return ESP_OK;
}

esp_err_t radio_mux_start(void)
{
    ESP_RETURN_ON_FALSE(!atomic_load(&g_mux.started), ESP_ERR_INVALID_STATE, TAG, "already started");
    g_mux.lock = xSemaphoreCreateMutex();
    g_mux.queue = xQueueCreate(QUEUE_LEN, sizeof(mux_msg_t));
    ESP_RETURN_ON_FALSE(g_mux.lock && g_mux.queue, ESP_ERR_NO_MEM, TAG, "queue and lock");
    ESP_RETURN_ON_ERROR(port_init(&g_mux.c6, RADIO_MUX_KIND_C6, false), TAG, "c6 port");
    ESP_RETURN_ON_ERROR(port_init(&g_mux.usb, RADIO_MUX_KIND_USB, true), TAG, "USB port");
    mux_load_chans();
    atomic_store(&g_mux.started, true);
    if (xTaskCreatePinnedToCore(owner_task, "radio_mux", OWNER_STACK, NULL, OWNER_PRIORITY, NULL, MGMT_CORE) !=
            pdPASS) {
        atomic_store(&g_mux.started, false);
        return ESP_ERR_NO_MEM;
    }
    size_t n = 0;

    for (size_t i = 0; i < RADIO_MUX_CHANNELS_MAX; i++) {
        n += g_mux.chans[i].used;
    }
    ESP_LOGI(TAG, "started: %u channels known, %u consumers", (unsigned)n, (unsigned)g_mux.n_consumers);
    return ESP_OK;
}

bool mux_post(const mux_msg_t *m)
{
    return g_mux.queue && xQueueSend(g_mux.queue, m, 0) == pdTRUE;
}