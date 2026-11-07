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
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_eventfd.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "radio_mux_priv.h"
#include "rcp_sync.h"

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
#define C6_BUSY_REASON "a C6 job holds the radio"
#define C6_JOB_FAILED_REASON "the last C6 job left the C6 not answering"

// At least one tick: at CONFIG_FREERTOS_HZ 100, pdMS_TO_TICKS(10) is 1 and smaller waits would not block.
#define WAIT_TICKS(ms) (pdMS_TO_TICKS(ms) > 0 ? pdMS_TO_TICKS(ms) : 1)

mux_t g_mux;

static void feed(mux_port_t *p, radio_mux_attach_ev_t ev);
static void c6_apply(bool reattach);

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

struct radio_mux_chan *mux_handle_chan(const radio_mux_chan_t *h, mux_port_t **port, uint32_t *gen)
{
    size_t slot;

    _Static_assert(RADIO_MUX_CHANNELS_MAX <= RADIO_MUX_HANDLE_SLOTS, "a handle names every channel slot");
    if (!radio_mux_handle_unpack((uintptr_t)h, &slot, gen) || slot >= RADIO_MUX_CHANNELS_MAX) {
        return NULL;
    }
    *port = slot == 0 ? &g_mux.c6 : &g_mux.usb; // c6 is slot 0, the USB channels the others
    return &g_mux.chans[slot];
}

radio_mux_chan_t *mux_new_attach(mux_port_t *p)
{
    xSemaphoreTake(p->tx_lock, portMAX_DELAY);
    xSemaphoreTake(p->ev_lock, portMAX_DELAY);
    mux_drain_evq(p);
    g_mux.gen = radio_mux_gen_next(g_mux.gen);
    atomic_store(&p->att_gen, g_mux.gen); // from here on the queue, ring and TX are the new handle's
    xSemaphoreGive(p->ev_lock);
    xSemaphoreGive(p->tx_lock);
    xSemaphoreTake(p->rx_lock, portMAX_DELAY);
    p->bound = NULL; // after the generation: a bind through an earlier handle that got in first does not stick
    xSemaphoreGive(p->rx_lock);
    p->att_handle = (radio_mux_chan_t *)radio_mux_handle_pack((size_t)(p->chan - g_mux.chans), g_mux.gen);
    return p->att_handle;
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
    if (p->kind == RADIO_MUX_KIND_C6 && (p->busy || !ch->present)) { // a job holds it, or the probe found no RCP
        ch->eff_mode = p->busy ? ch->mode : RADIO_MUX_MODE_UNAVAILABLE;
        ch->state = p->busy ? RADIO_MUX_STATE_BUSY : RADIO_MUX_STATE_IDLE;
        snprintf(ch->reason, sizeof(ch->reason), "%s", p->busy ? C6_BUSY_REASON : p->fault);
        mux_unlock();
        return;
    }
    if (p->leased) { // OpenThread is being started on it (a move): its stored mode changes only once that runs
        ch->eff_mode = RADIO_MUX_MODE_OTBR;
        ch->state = RADIO_MUX_STATE_STARTING;
        ch->reason[0] = '\0';
        mux_unlock();
        return;
    }
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
    radio_mux_chan_t *h = mux_new_attach(p);

    xSemaphoreTake(p->rx_lock, portMAX_DELAY);
    radio_mux_reset_count_clear(&p->reset_count); // a new attach starts with no unanswered reset
    xSemaphoreGive(p->rx_lock);
    p->consumer = c;
    p->tx_frames = radio_mux_port_frames(p->kind == RADIO_MUX_KIND_C6, c->frames); // the C6 ignores c->frames
    mux_set_rx(p, p->tx_frames ? MUX_RX_FRAMES : MUX_RX_RAW);
    atomic_store(&p->tx_ok, true);
    const esp_err_t err = c->attach(h, c->ctx);

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
    taskENTER_CRITICAL(&p->lock); // after tx_ok: a late radio_mux_set_busy_peer(true) is refused (R26)
    p->busy_peer = false;
    taskEXIT_CRITICAL(&p->lock);
    mux_set_rx(p, MUX_RX_DISCARD);
    if (!c) {
        return;
    }
    mux_post_ctl(p, RADIO_MUX_EV_GONE);
    c->detach(p->att_handle, c->ctx);
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
    if (p->leased) { // the lease holder takes its GONE from the channel's own queue
        p->leased = false;
        mux_post_ctl(p, RADIO_MUX_EV_GONE);
    }
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

/* ---- owner: the C6 ---- */

// Whether rcp_sync's last probe leaves the C6 listed (radio_mux_c6_present); why not in `why`.
static bool c6_probe_present(char *why, size_t cap)
{
    rcp_sync_probe_result_t r;

    rcp_sync_get_last_probe(&r);
    const radio_mux_c6_probe_t probe = {
        .updater_reachable = r.updater_reachable,
        .installed_valid = r.installed_valid,
        .esp_hosted = r.esp_hosted,
        .rom_recovery_failed = r.rom_recovery_failed,
    };

    return radio_mux_c6_present(&probe, why, cap);
}

// Lists c6 present (firmware spinel unless a record says otherwise) or unavailable with `why`. Not while attached.
static void c6_list(bool present, const char *why)
{
    mux_port_t *p = &g_mux.c6;
    struct radio_mux_chan *ch = &g_mux.chans[0];

    mux_lock();
    ch->present = present;
    ch->supported = true;
    if (present) {
        ch->last_seen = mux_now_epoch();
        if (ch->fw.rec.type == RADIO_MUX_FW_UNKNOWN) {
            ch->fw.rec.type = RADIO_MUX_FW_SPINEL; // the on-board RCP; a consumer's note may refine the record
        }
    }
    mux_unlock();
    snprintf(p->fault, sizeof(p->fault), "%s", present ? "" : why);
    p->att.state = present ? RADIO_MUX_AS_IDLE : RADIO_MUX_AS_ABSENT;
    if (!present) {
        ESP_LOGW(TAG, "%s unavailable: %s", ch->id, why);
    }
}

// Owner: the C6 to what its channel wants (R8). Open, with the stored mode's consumer attached, while that consumer is
// registered and the C6 listed present; else closed: held in reset in mode off, left running otherwise (before an otbr
// consumer exists, OpenThread's own SPI path uses it). `reattach`: an attached consumer is detached (GONE) and the
// stored mode's attached again, the C6 staying open. Nothing while a job holds it or it is unavailable.
static void c6_apply(bool reattach)
{
    mux_port_t *p = &g_mux.c6;
    char msg[RADIO_MUX_REASON_MAX];

    if (p->busy || p->leased) {
        refresh(p); // the job owns the C6 and its pins, or OpenThread is being started on it (a lease)
        return;
    }
    if (p->att.state == RADIO_MUX_AS_ABSENT) { // unavailable: nothing to attach, but off still holds it in reset
        if (p->chan->mode == RADIO_MUX_MODE_OFF) {
            c6_transport_close();
        }
        refresh(p);
        return;
    }
    const bool want = want_attach(p);

    if (p->att.state != RADIO_MUX_AS_ATTACHED) {
        p->fault[0] = '\0'; // a failed open or attach is tried again (or no longer applies)
    }
    if (p->att.state == RADIO_MUX_AS_ATTACHED && (!want || reattach)) {
        do_detach(p);
        p->att.state = RADIO_MUX_AS_IDLE;
    }
    if (want && p->att.state != RADIO_MUX_AS_ATTACHED) {
        if (c6_transport_open(msg, sizeof(msg)) != ESP_OK) {
            snprintf(p->fault, sizeof(p->fault), "%s", msg);
            ESP_LOGW(TAG, "%s: %s", p->chan->id, msg);
        } else {
            p->att.state = RADIO_MUX_AS_ATTACHED;
            do_attach(p); // a failure feeds ATTACH_FAILED: IDLE with the reason in p->fault
        }
    }
    if (p->att.state != RADIO_MUX_AS_ATTACHED) {
        if (p->chan->mode == RADIO_MUX_MODE_OFF) {
            c6_transport_close();
        } else {
            c6_transport_release();
        }
    }
    refresh(p);
}

// Owner task start: c6 listed from rcp_sync's last probe (none yet: present), then attached when its mode wants it.
static void c6_init(void)
{
    char why[RADIO_MUX_REASON_MAX];

    g_mux.c6.chan = &g_mux.chans[0];
    c6_list(c6_probe_present(why, sizeof(why)), why);
    c6_apply(false);
}

// A C6 job (push, ROM recovery, probe) takes the C6: refused while rcp-proxy has a peer (409 c6_busy) or another job
// runs; otherwise GONE to its consumer, SPI2, INT and the reset GPIO released (the C6 left running) for rcp_sync.
static void handle_c6_job_begin(mux_call_t *call)
{
    mux_port_t *p = &g_mux.c6;
    bool peer;

    taskENTER_CRITICAL(&p->lock);
    peer = p->busy_peer;
    taskEXIT_CRITICAL(&p->lock);
    if (p->busy || p->leased ||
            radio_mux_c6_job_refused(p->chan->mode, p->att.state == RADIO_MUX_AS_ATTACHED, peer)) {
        ESP_LOGW(TAG, "c6: job refused: %s", p->busy ? "another job holds the C6" :
                 p->leased ? "OpenThread is being moved to it" : "rcp-proxy has a peer");
        call->err = ESP_ERR_INVALID_STATE;
        return;
    }
    if (p->att.state == RADIO_MUX_AS_ATTACHED) {
        do_detach(p);
    }
    c6_transport_release();
    p->busy = true;
    p->att.state = RADIO_MUX_AS_ABSENT;
    refresh(p);
    call->err = ESP_OK;
    ESP_LOGI(TAG, "c6: a job holds the radio");
}

// The job's end: c6 listed again from rcp_sync's last probe (unavailable when the job says the C6 does not work),
// then its consumer attached again (SPI2 opened). Without a job: the listing is refreshed when nothing is attached.
static void handle_c6_job_end(mux_call_t *call)
{
    mux_port_t *p = &g_mux.c6;
    char why[RADIO_MUX_REASON_MAX];

    call->err = ESP_OK;
    if (!p->busy && p->att.state == RADIO_MUX_AS_ATTACHED) {
        return; // no job ran: what is attached stays
    }
    p->busy = false;
    bool present = c6_probe_present(why, sizeof(why));

    if (present && !call->c6_ok) {
        present = false;
        snprintf(why, sizeof(why), "%s", C6_JOB_FAILED_REASON);
    }
    c6_list(present, why);
    c6_apply(false);
    ESP_LOGI(TAG, "c6: the job ended (%s)", present ? "present" : why);
}

/* ---- owner: the OTBR lease (a move, R9) ---- */

// Takes a present channel with no consumer for OTBR before its mode is stored: frames go to its own queue and
// eventfd, TX takes frames. The C6 is opened (out of reset); a USB device is open already (IDLE).
static void handle_lease(mux_call_t *call)
{
    struct radio_mux_chan *ch = find_chan(call->id);
    mux_port_t *p = ch ? ch->port : NULL;
    const char *why = NULL;
    char msg[RADIO_MUX_REASON_MAX];

    call->handle = NULL;
    if (!ch) {
        call->err = ESP_ERR_NOT_FOUND;
        snprintf(call->reason, sizeof(call->reason), "no channel %s", call->id);
        return;
    }
    if (p->chan != ch || !ch->present || !ch->supported) {
        why = "not detected";
    } else if (p->busy) {
        why = C6_BUSY_REASON;
    } else if (p->leased) {
        why = "OpenThread is already being moved to it";
    } else if (p->att.state == RADIO_MUX_AS_ATTACHED || p->consumer) {
        why = ch->mode == RADIO_MUX_MODE_RCP_PROXY ? "rcp-proxy runs on it: set it off first" : "in use";
    } else if (p->att.state != RADIO_MUX_AS_IDLE || (p->kind == RADIO_MUX_KIND_USB && !usb_transport_is_open())) {
        why = p->fault[0] ? p->fault : "not open";
    } else if (!radio_mux_fits(&g_mux.rules, RADIO_MUX_MODE_OTBR, ch->fw.rec.type)) {
        why = "its firmware is not spinel";
    }
    if (why) {
        call->err = ESP_ERR_INVALID_STATE;
        snprintf(call->reason, sizeof(call->reason), "%s", why);
        return;
    }
    if (p->kind == RADIO_MUX_KIND_C6 && c6_transport_open(msg, sizeof(msg)) != ESP_OK) {
        call->err = ESP_FAIL;
        snprintf(call->reason, sizeof(call->reason), "%s", msg);
        return;
    }
    radio_mux_chan_t *h = mux_new_attach(p);

    p->consumer = NULL;
    p->tx_frames = true;
    mux_set_rx(p, MUX_RX_FRAMES);
    atomic_store(&p->tx_ok, true);
    p->leased = true;
    p->fault[0] = '\0';
    refresh(p);
    call->handle = h;
    call->err = ESP_OK;
    ESP_LOGI(TAG, "%s: leased for OTBR", ch->id);
}

// Gives a leased channel back: delivery stops, the C6 returns to what its stored mode wants (off: reset). A fault
// text stays as the channel's reason (state fault) until its next mode change, plug-in or C6 job.
static void handle_lease_end(mux_call_t *call)
{
    mux_port_t *p;
    uint32_t gen;
    struct radio_mux_chan *ch = mux_handle_chan(call->handle, &p, &gen);

    call->err = ESP_OK;
    if (!ch || !p->leased || p->chan != ch || !radio_mux_handle_current(gen, atomic_load(&p->att_gen))) {
        return; // committed, or the device went away (GONE ended the lease)
    }
    p->leased = false;
    atomic_store(&p->tx_ok, false);
    mux_set_rx(p, MUX_RX_DISCARD);
    mux_drain_evq(p);
    if (p->kind == RADIO_MUX_KIND_C6) {
        c6_apply(false);
    }
    snprintf(p->fault, sizeof(p->fault), "%s", call->reason);
    refresh(p);
    ESP_LOGI(TAG, "%s: lease ended%s%s", ch->id, call->reason[0] ? ": " : "", call->reason);
}

// The end of a move: `to` (leased, OpenThread runs on it) is stored in otbr and `from` in off, in that order of
// effect: `from`'s consumer is detached (GONE), then the lease becomes the otbr consumer's attach (no GONE, no reopen).
// The end of a stop: `from` leaves otbr for the mode its PUT asked for (leave_mode), once OpenThread stopped on it;
// its otbr consumer is detached (GONE) and that mode's attached.
static void commit_stop(mux_call_t *call, struct radio_mux_chan *from)
{
    const radio_mux_mode_t old = from ? from->mode : RADIO_MUX_MODE_OFF;

    if (!from || old != RADIO_MUX_MODE_OTBR) {
        call->err = ESP_ERR_INVALID_STATE;
        return;
    }
    const radio_mux_mode_t m = from->leave_mode;

    mux_lock();
    from->mode = m == RADIO_MUX_MODE_RCP_PROXY || m == RADIO_MUX_MODE_ZIGGURAT ? m : RADIO_MUX_MODE_OFF;
    mux_unlock();
    call->err = mux_store_chan(from);
    if (call->err != ESP_OK) {
        mux_lock();
        from->mode = old;
        mux_unlock();
        return;
    }
    mux_port_t *p = from->port;

    if (p->kind == RADIO_MUX_KIND_C6) {
        c6_apply(true); // otbr detached (GONE); the new mode's consumer attached, or off: held in reset
    } else if (p->chan == from) {
        feed(p, RADIO_MUX_AEV_MODE_SET);
    }
    ESP_LOGI(TAG, "otbr stopped on %s: %s", from->id, radio_mux_mode_name(from->mode));
}

static void handle_commit_otbr(mux_call_t *call)
{
    struct radio_mux_chan *from = call->from[0] ? find_chan(call->from) : NULL;

    if (call->id[0] == '\0') { // no target: a stop
        commit_stop(call, from);
        return;
    }
    struct radio_mux_chan *to = find_chan(call->id);
    const radio_mux_consumer_t *c = mux_find_consumer(RADIO_MUX_MODE_OTBR);

    if (!to || !to->port->leased || to->port->chan != to || !c) {
        call->err = ESP_ERR_INVALID_STATE;
        return;
    }
    const radio_mux_mode_t to_old = to->mode;
    const radio_mux_mode_t from_old = from ? from->mode : RADIO_MUX_MODE_OFF;

    mux_lock();
    to->mode = RADIO_MUX_MODE_OTBR;
    if (from && from != to) {
        from->mode = RADIO_MUX_MODE_OFF;
    }
    mux_unlock();
    call->err = mux_store_chan(to);
    if (call->err == ESP_OK && from && from != to) {
        call->err = mux_store_chan(from);
    }
    if (call->err != ESP_OK) {
        mux_lock();
        to->mode = to_old;
        if (from && from != to) {
            from->mode = from_old;
        }
        mux_unlock();
        mux_store_chan(to); // best effort: back to what was stored
        return;
    }
    if (from && from != to) {
        mux_port_t *fp = from->port;

        if (fp->kind == RADIO_MUX_KIND_C6) {
            c6_apply(false); // otbr detached (GONE), the C6 held in reset (off)
        } else if (fp->chan == from) {
            feed(fp, RADIO_MUX_AEV_MODE_SET);
        }
    }
    mux_port_t *p = to->port;

    p->leased = false;
    p->consumer = c;
    p->att.state = RADIO_MUX_AS_ATTACHED;
    const esp_err_t err = c->attach(p->att_handle, c->ctx); // the lease's handle: the same generation goes on

    if (err != ESP_OK) {
        atomic_store(&p->tx_ok, false);
        mux_set_rx(p, MUX_RX_DISCARD);
        mux_drain_evq(p);
        p->consumer = NULL;
        snprintf(p->fault, sizeof(p->fault), "otbr attach failed: %s", esp_err_to_name(err));
        p->att.state = RADIO_MUX_AS_IDLE;
    }
    refresh(p);
    ESP_LOGI(TAG, "otbr moved%s%s to %s", from ? " from " : "", from ? from->id : "", to->id);
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
    if (radio_mux_proxy_merge(&ch->proxy, req->has_proxy ? req->proxy_fields : 0, &req->proxy, proxy) !=
            RADIO_MUX_PROXY_CHECK_OK) {
        return RADIO_MUX_REFUSE_BAD_VALUE;
    }
    if (ch->kind == RADIO_MUX_KIND_C6 && (req->has_baud || req->has_flow)) {
        return RADIO_MUX_REFUSE_BAD_VALUE; // SPI: the C6 has no line settings
    }
    if (req->has_baud && !radio_mux_baud_ok(req->baud)) {
        return RADIO_MUX_REFUSE_BAD_VALUE;
    }
    return RADIO_MUX_APPLY;
}

// On MOVE: `from` gets the channel OpenThread leaves ("" for a start), *stop whether it is a stop on `ch` itself.
static radio_mux_verdict_t check_mode(struct radio_mux_chan *ch, radio_mux_mode_t want, char *from, size_t cap,
                                      bool *stop)
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

    *stop = false;
    if (v == RADIO_MUX_MOVE) {
        snprintf(from, cap, "%s", move_from < n ? views[move_from].id : "");
        *stop = move_from == target;
    }
    return v;
}

static void handle_set(mux_call_t *call)
{
    struct radio_mux_chan *ch = find_chan(call->id);
    const radio_mux_set_req_t *req = &call->req;
    radio_mux_proxy_settings_t proxy;

    call->from[0] = '\0';
    call->stop = false;
    if (!ch) {
        call->verdict = RADIO_MUX_REFUSE_UNKNOWN_CHANNEL;
        return;
    }
    if (ch->port->leased && ch->port->chan == ch) {
        call->verdict = RADIO_MUX_REFUSE_BUSY; // OpenThread is being moved to it
        return;
    }
    radio_mux_verdict_t v = check_req(ch, req, &proxy);

    if (v == RADIO_MUX_APPLY && req->has_mode) {
        v = check_mode(ch, req->mode, call->from, sizeof(call->from), &call->stop);
        if (v == RADIO_MUX_MOVE && !call->have_mover) {
            v = RADIO_MUX_REFUSE_NOT_AVAILABLE; // nothing runs a move yet (Task 7)
        }
        if (v == RADIO_MUX_MOVE && (req->has_baud || req->has_flow || req->has_proxy)) {
            v = RADIO_MUX_REFUSE_BAD_VALUE; // a start, move or stop goes alone: its settings would never be stored
        }
        if (v == RADIO_MUX_MOVE && call->stop) {
            ch->leave_mode = req->mode; // stored by the stop's commit, once OpenThread stopped
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

    if (v == RADIO_MUX_APPLY_REBOOT) {
        return; // stored: applies after the reboot
    }
    if (p->kind == RADIO_MUX_KIND_C6) {
        // As on USB: a new mode or new rcp_proxy settings (in rcp-proxy) re-attach, a PUT of the mode retries a fault.
        // While a job holds the C6 or it is unavailable, the change is stored and applies at the job's end.
        if (mode_changed || (proxy_changed && ch->mode == RADIO_MUX_MODE_RCP_PROXY) || req->has_mode) {
            c6_apply(mode_changed || (proxy_changed && ch->mode == RADIO_MUX_MODE_RCP_PROXY));
        }
        return;
    }
    if (p->chan != ch) {
        return; // stored: applies when the channel returns
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

// A reset: a dongle is closed and reopened over USB, then RESET; GONE (detach) when it does not reopen, or when this is
// the RADIO_MUX_USB_DEAD_RESETS-th reset in a row with nothing received in between (closed, fault "no answer after 3
// resets"; an unplug or a PUT of the mode opens it again). The C6 gets its reset GPIO pulse.
static void handle_reset(radio_mux_chan_t *h)
{
    mux_port_t *p;
    uint32_t gen;
    struct radio_mux_chan *ch = mux_handle_chan(h, &p, &gen);

    if (!ch || p->chan != ch || p->att.state != RADIO_MUX_AS_ATTACHED ||
            !radio_mux_handle_current(gen, atomic_load(&p->att_gen))) {
        return; // a channel that is not attached has nobody to tell; an earlier attach's reset is not the current one's
    }
    if (p->kind == RADIO_MUX_KIND_C6) { // the reset GPIO pulse; the C6 task posts RESET when the RCP answers
        if (c6_transport_reset() == ESP_OK) {
            taskENTER_CRITICAL(&p->lock);
            p->ctr.resets++;
            taskEXIT_CRITICAL(&p->lock);
            ESP_LOGI(TAG, "%s: reset", ch->id);
        }
        return;
    }
    xSemaphoreTake(p->rx_lock, portMAX_DELAY);
    const bool give_up = radio_mux_reset_count_request(&p->reset_count);
    xSemaphoreGive(p->rx_lock);

    atomic_store(&p->tx_ok, false);
    mux_set_rx(p, MUX_RX_DISCARD);
    usb_transport_close(p);
    if (give_up) {
        snprintf(p->fault, sizeof(p->fault), "%s", RADIO_MUX_USB_DEAD_REASON);
        ESP_LOGW(TAG, "%s: %s: closed (unplug it or PUT its mode to open it again)", ch->id, p->fault);
        feed(p, RADIO_MUX_AEV_MODE_SET); // not open: the machine detaches the consumer (GONE), the channel shows fault
        return;
    }
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

static void handle_note_fw(radio_mux_chan_t *h, const radio_mux_fw_record_t *fw)
{
    mux_port_t *p;
    uint32_t gen;
    struct radio_mux_chan *ch = mux_handle_chan(h, &p, &gen);

    if (!ch) {
        return;
    }
    if (p->chan != ch || p->att.state != RADIO_MUX_AS_ATTACHED ||
            !radio_mux_handle_current(gen, atomic_load(&p->att_gen))) {
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
        handle_reset(m->h);
        break;
    case MUX_MSG_NOTE_FW:
        handle_note_fw(m->note.h, &m->note.fw);
        break;
    case MUX_MSG_C6_JOB_BEGIN: {
        int pending = MUX_CALL_PENDING;

        // A caller that timed out marked the call abandoned and returned ESP_ERR_TIMEOUT: nothing may run then, or c6
        // would stay busy with no job_end to come.
        if (atomic_compare_exchange_strong(&m->call->claim, &pending, MUX_CALL_TAKEN)) {
            handle_c6_job_begin(m->call);
        } else {
            ESP_LOGW(TAG, "c6: job begin given up by its caller: skipped");
        }
        xSemaphoreGive(m->call->done);
        mux_call_put(m->call);
        break;
    }
    case MUX_MSG_C6_JOB_END:
        handle_c6_job_end(m->call);
        xSemaphoreGive(m->call->done);
        mux_call_put(m->call);
        break;
    case MUX_MSG_LEASE_END:
        handle_lease_end(m->call);
        xSemaphoreGive(m->call->done);
        mux_call_put(m->call);
        break;
    case MUX_MSG_LEASE:
    case MUX_MSG_COMMIT_OTBR: {
        int pending = MUX_CALL_PENDING;

        // A lease or commit whose caller gave up does not run: nobody would end the lease, and the caller has already
        // gone back to the source (it then ends the lease, which always runs).
        if (atomic_compare_exchange_strong(&m->call->claim, &pending, MUX_CALL_TAKEN)) {
            if (m->type == MUX_MSG_LEASE) {
                handle_lease(m->call);
            } else {
                handle_commit_otbr(m->call);
            }
        } else {
            ESP_LOGW(TAG, "%s: lease call given up by its caller: skipped", m->call->id);
        }
        xSemaphoreGive(m->call->done);
        mux_call_put(m->call);
        break;
    }
    }
}

static void owner_task(void *arg)
{
    mux_msg_t m;
    const esp_err_t err = usb_transport_start();

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "USB host start: %s, no USB radios", esp_err_to_name(err));
    }
    // R13 at start: every present channel whose stored mode has a consumer: the C6 here, USB devices attached at boot
    // through new_dev_cb like any plug-in.
    c6_init();
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

// The queue, locks and eventfd of a transport. Its buffers (frame pool, discard and TX buffers, the USB ring) come with
// its first open: a board without a dongle never allocates the USB ones.
static esp_err_t port_init(mux_port_t *p, radio_mux_kind_t kind)
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
    p->ev_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(p->evq && p->rx_lock && p->tx_lock && p->ev_lock, ESP_ERR_NO_MEM, TAG, "port queue and locks");
    p->efd = eventfd(0, 0);
    ESP_RETURN_ON_FALSE(p->efd >= 0, ESP_ERR_NO_MEM, TAG, "eventfd (is the eventfd VFS registered?)");
    return ESP_OK;
}

esp_err_t radio_mux_start(void)
{
    ESP_RETURN_ON_FALSE(!atomic_load(&g_mux.started), ESP_ERR_INVALID_STATE, TAG, "already started");
    g_mux.lock = xSemaphoreCreateMutex();
    g_mux.queue = xQueueCreate(QUEUE_LEN, sizeof(mux_msg_t));
    ESP_RETURN_ON_FALSE(g_mux.lock && g_mux.queue, ESP_ERR_NO_MEM, TAG, "queue and lock");
    ESP_RETURN_ON_ERROR(port_init(&g_mux.c6, RADIO_MUX_KIND_C6), TAG, "c6 port");
    ESP_RETURN_ON_ERROR(port_init(&g_mux.usb, RADIO_MUX_KIND_USB), TAG, "USB port");
    mux_load_chans();
    ESP_RETURN_ON_ERROR(c6_transport_start(), TAG, "c6 task");
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