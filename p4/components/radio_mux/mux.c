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

static const char *TAG = "radio_mux";

#define OWNER_PRIORITY 6
#define OWNER_STACK 6144
#define QUEUE_LEN 12
#define DETECT_POLL_MS 10
#define IDLE_POLL_MS 1000
#define USB_DUE_POLL_MS 100 // while a USB close is retried or a transfer error waits for its reopen
#define FULL_LOG_US (60LL * 1000 * 1000) // "channel list full" at most once a minute
#define PRESENCE_POLL_US 1000000 // how often a device that is not open is checked for still being there
#define DETECT_CHUNK 256
#define EVQ_LEN (RADIO_MUX_POOL_SLOTS + 4) // frames hold a slot each, so control events always fit
#define DEFAULT_BAUD 460800
#define EPOCH_VALID 1577836800 // 2020-01-01: an earlier clock was never set
#define C6_JOB_FAILED_REASON "the last C6 job left the C6 not answering"
#define BOOT_TASK_STACK 6144 // rcp_sync's probe and ROM recovery ran on net_init's 4 KB before
#define BOOT_TASK_PRIORITY 5

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

    return radio_mux_attach_wanted(&g_mux.rules, &v, orch_find(ch->mode) != NULL);
}

// Why the C6 is busy (in reinit): its maintenance, in the texts the API showed before the orchestrator.
static const char *c6_busy_reason(const mux_port_t *p)
{
    return radio_mux_busy_reason(p->work);
}

// Whether the C6 is in reinit or about to enter it (its bound instance was asked to let go).
static bool c6_maintained(const mux_port_t *p)
{
    return p->busy || g_mux.reinit.phase != RADIO_MUX_RP_IDLE;
}

// Owner, under mux_lock: the channel's lifecycle state and reason (design section 4.1), from the port.
static void set_chan_state(mux_port_t *p, struct radio_mux_chan *ch)
{
    radio_mux_chan_state_t cs = RADIO_MUX_CS_READY;
    radio_mux_fault_kind_t kind = RADIO_MUX_FAULT_NONE;
    radio_mux_work_t work = RADIO_MUX_WORK_NONE;
    const char *why = "";

    if (p->kind == RADIO_MUX_KIND_BUILTIN && p->busy) {
        cs = RADIO_MUX_CS_REINIT;
        work = p->work;
    } else if (p->kind == RADIO_MUX_KIND_BUILTIN && !ch->present) {
        cs = RADIO_MUX_CS_FAULT;
        kind = RADIO_MUX_FAULT_FIRMWARE;
        why = p->fault;
    } else if (!p->leased) {
        switch (p->att.state) {
        case RADIO_MUX_AS_PROBING:
        case RADIO_MUX_AS_REOPENING:
            cs = RADIO_MUX_CS_REINIT;
            work = RADIO_MUX_WORK_DETECT;
            break;
        case RADIO_MUX_AS_IDLE:
            if (p->fault[0] != '\0' && p->fault_kind != RADIO_MUX_FAULT_NONE) {
                cs = RADIO_MUX_CS_FAULT;
                kind = p->fault_kind;
                why = p->fault;
            }
            break;
        case RADIO_MUX_AS_ABSENT:
        case RADIO_MUX_AS_CLOSING:
            cs = RADIO_MUX_CS_ABSENT;
            break;
        case RADIO_MUX_AS_ATTACHED:
            break;
        }
    }
    ch->chan_state = cs;
    ch->fault_kind = kind;
    ch->work = work;
    snprintf(ch->chan_reason, sizeof(ch->chan_reason), "%s", cs == RADIO_MUX_CS_REINIT ? radio_mux_work_reason(work) :
             why);
}

// Owner: what the API shows for the channel on port p (mode, state, reason; channel_state), from the attach state.
static void refresh(mux_port_t *p)
{
    struct radio_mux_chan *ch = p->chan;

    if (!ch) {
        return;
    }
    mux_lock();
    set_chan_state(p, ch);
    if (p->kind == RADIO_MUX_KIND_BUILTIN && (p->busy || !ch->present)) { // in reinit, or the probe found no RCP
        ch->eff_mode = p->busy ? ch->mode : RADIO_MUX_MODE_UNAVAILABLE;
        ch->state = p->busy ? RADIO_MUX_STATE_BUSY : RADIO_MUX_STATE_IDLE;
        snprintf(ch->reason, sizeof(ch->reason), "%s", p->busy ? c6_busy_reason(p) : p->fault);
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
            if (why[0] != '\0') { // the stored mode's instance is not bound: the orchestrator says why
                snprintf(ch->mode_state, sizeof(ch->mode_state), "no_radio");
                snprintf(ch->mode_reason, sizeof(ch->mode_reason), "%s", why);
            }
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
    if (ch->det_failed && ch->reason[0] == '\0' && ch->state != RADIO_MUX_STATE_STARTING) {
        snprintf(ch->reason, sizeof(ch->reason), "%s", RADIO_MUX_DETECT_FAILED_REASON);
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
    p->fault_kind = ok ? RADIO_MUX_FAULT_NONE : RADIO_MUX_FAULT_LINK;
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
    if (!ok) {
        p->fault_kind = RADIO_MUX_FAULT_LINK;
    }
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

// The stored mode's instance is bound (BIND, or BACK when it holds the binding) with a new handle. The receive path
// runs before the input, so nothing the instance asks for is lost: frames that arrive before it binds its queue wait in
// the channel's queue and move over in radio_mux_bind_queue.
static void do_attach(mux_port_t *p)
{
    struct radio_mux_chan *ch = p->chan;
    orch_inst_t *oi = orch_find(ch->mode);

    if (!oi) { // want_attach said yes, so this does not happen; keep the machine consistent anyway
        snprintf(p->fault, sizeof(p->fault), "no %s consumer", radio_mux_mode_name(ch->mode));
        p->fault_kind = RADIO_MUX_FAULT_NONE;
        feed(p, RADIO_MUX_AEV_ATTACH_FAILED);
        return;
    }
    radio_mux_chan_t *h = mux_new_attach(p);

    xSemaphoreTake(p->rx_lock, portMAX_DELAY);
    radio_mux_reset_count_clear(&p->reset_count); // a new attach starts with no unanswered reset
    xSemaphoreGive(p->rx_lock);
    p->inst = oi;
    p->tx_frames = radio_mux_port_frames(p->kind == RADIO_MUX_KIND_BUILTIN, oi->inst.frames); // the C6 ignores `frames`
    mux_set_rx(p, p->tx_frames ? MUX_RX_FRAMES : MUX_RX_RAW);
    atomic_store(&p->tx_ok, true);
    p->fault[0] = '\0';
    orch_bind(p, oi, h);
    ESP_LOGI(TAG, "%s: %s bound", ch->id, radio_mux_mode_name(ch->mode));
}

// The bound instance lets go: GONE into its queue, then UNBIND (the binding goes: a mode change) or LOST (it stays:
// the channel went away or failed, with `kind` and `reason`).
static void do_detach(mux_port_t *p, radio_mux_input_type_t type, radio_mux_fault_kind_t kind, const char *reason)
{
    orch_inst_t *oi = p->inst;

    atomic_store(&p->tx_ok, false);
    mux_set_rx(p, MUX_RX_DISCARD);
    if (!oi) {
        return;
    }
    mux_post_ctl(p, RADIO_MUX_EV_GONE);
    orch_leave(p, oi, type, kind, reason);
    p->inst = NULL;
    xSemaphoreTake(p->rx_lock, portMAX_DELAY);
    p->bound = NULL;
    xSemaphoreGive(p->rx_lock);
    ESP_LOGI(TAG, "%s: %s let go (%s)", p->chan->id, radio_mux_mode_name(oi->inst.mode),
             type == RADIO_MUX_IN_UNBIND ? "unbound" : "lost");
}

// The bound instance lets go because the channel cannot be used now (closed, failed): UNBIND when the stored mode is
// no longer its mode (a mode change), else LOST with the port's fault (or "not detected").
static void detach_for_channel(mux_port_t *p)
{
    if (p->inst && p->chan && p->inst->inst.mode != p->chan->mode) {
        do_detach(p, RADIO_MUX_IN_UNBIND, RADIO_MUX_FAULT_NONE, NULL);
        return;
    }
    do_detach(p, RADIO_MUX_IN_LOST, p->fault[0] ? p->fault_kind : RADIO_MUX_FAULT_NONE,
              p->fault[0] ? p->fault : "not detected");
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
    ch->chan_state = RADIO_MUX_CS_ABSENT;
    ch->fault_kind = RADIO_MUX_FAULT_NONE;
    ch->work = RADIO_MUX_WORK_NONE;
    ch->chan_reason[0] = '\0';
    p->chan = NULL;
    mux_unlock();
    if (ch->remembered) {
        mux_store_chan(ch);
    }
    ESP_LOGI(TAG, "%s: unplugged", ch->id);
}

// A free channel slot for a new USB device (radio_mux_evict_pick): an unused one, or the oldest unplugged channel in
// mode off, which is forgotten. NULL (logged at most once a minute) when none can be had: the device is not listed.
static struct radio_mux_chan *evict_slot(const char *id, bool for_unsupported)
{
    static int64_t full_log_us;
    radio_mux_slot_view_t views[RADIO_MUX_CHANNELS_MAX - 1];

    for (size_t i = 1; i < RADIO_MUX_CHANNELS_MAX; i++) { // slot 0 is the C6
        const struct radio_mux_chan *ch = &g_mux.chans[i];

        views[i - 1] = (radio_mux_slot_view_t) {
            .used = ch->used, .present = ch->present, .on_port = ch->used && ch->port && ch->port->chan == ch,
            .remembered = ch->remembered,
            .mode = ch->mode, .last_seen = ch->last_seen,
        };
    }
    const size_t n = RADIO_MUX_CHANNELS_MAX - 1;
    const size_t pick = radio_mux_evict_pick(views, n, for_unsupported);

    if (pick == n) {
        const int64_t now = esp_timer_get_time();

        if (full_log_us == 0 || now - full_log_us >= FULL_LOG_US) {
            full_log_us = now;
            ESP_LOGW(TAG, "%s not listed: the channel list is full (forget an unplugged channel to make room)", id);
        }
        return NULL;
    }
    struct radio_mux_chan *ch = &g_mux.chans[pick + 1];

    if (ch->used) {
        ESP_LOGW(TAG, "%s forgotten: room for a new channel", ch->id);
        mux_erase_chan(ch);
        mux_lock();
        ch->used = false;
        mux_unlock();
    }
    orch_forget_slot((size_t)(ch - g_mux.chans));
    return ch;
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
    ch = evict_slot(id, false);
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

    if (!ch) { // the channel list is full (evict_slot logged it)
        feed(p, RADIO_MUX_AEV_UNPLUG); // back to ABSENT through CLOSING; nothing is open
        return;
    }
    p->dev = p->pending;
    p->fault[0] = '\0';
    p->warn[0] = '\0';
    mux_lock();
    p->chan = ch;
    ch->det_failed = false;
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

    if (radio_mux_detect_keeps_record(ch->fw.rec.type, rec->type)) {
        mux_lock();
        ch->det_failed = true;
        mux_unlock();
        ESP_LOGW(TAG, "%s: the firmware detection found nothing: keeping the earlier record (%s %s)", ch->id,
                 radio_mux_fw_name(ch->fw.rec.type), ch->fw.rec.version);
        feed(p, RADIO_MUX_AEV_DETECT_DONE);
        return;
    }
    mux_lock();
    ch->det_failed = false;
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
        detach_for_channel(p);
        break;
    case RADIO_MUX_ACT_REATTACH: // a new mode, or new rcp_proxy settings: a new binding
        do_detach(p, RADIO_MUX_IN_UNBIND, RADIO_MUX_FAULT_NONE, NULL);
        do_attach(p);
        break;
    case RADIO_MUX_ACT_GONE_CLOSE: // unplugged
        do_detach(p, RADIO_MUX_IN_LOST, RADIO_MUX_FAULT_NONE, "not detected");
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

// Whether the built-in RCP's last probe leaves the C6 listed (its `present` op); why not in `why`. No built-in RCP
// described: listed (its installed firmware is tried).
static bool c6_probe_present(char *why, size_t cap)
{
    const radio_mux_builtin_t *b = g_mux.builtin;
    radio_mux_fault_kind_t kind;

    if (b == NULL) {
        snprintf(why, cap, "%s", "");
        return true;
    }
    return b->present(&kind, why, cap);
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
    p->fault_kind = present ? RADIO_MUX_FAULT_NONE : RADIO_MUX_FAULT_FIRMWARE;
    p->att.state = present ? RADIO_MUX_AS_IDLE : RADIO_MUX_AS_ABSENT;
    if (!present) {
        ESP_LOGW(TAG, "%s unavailable: %s", ch->id, why);
    }
}

// Owner: the C6 to what its channel wants (R8). Open, with the stored mode's consumer attached, while that consumer is
// registered and the C6 listed present; else closed: held in reset in mode off, left running otherwise (before an otbr
// consumer exists, OpenThread's own SPI path uses it). `reattach`: an attached consumer is detached (GONE) and the
// stored mode's attached again, the C6 staying open. Nothing while it is in reinit or unavailable.
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
        if (reattach) {
            do_detach(p, RADIO_MUX_IN_UNBIND, RADIO_MUX_FAULT_NONE, NULL);
        } else {
            detach_for_channel(p);
        }
        p->att.state = RADIO_MUX_AS_IDLE;
    }
    if (want && p->att.state != RADIO_MUX_AS_ATTACHED) {
        if (c6_transport_open(msg, sizeof(msg)) != ESP_OK) {
            snprintf(p->fault, sizeof(p->fault), "%s", msg);
            p->fault_kind = RADIO_MUX_FAULT_LINK;
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

// Owner task start: the C6 starts in reinit for its boot steps (the boot task runs them, then MAINT_DONE): busy,
// nothing bound, SPI2 and the pins untouched; its stored mode's instance is bound once, after the steps.
static void c6_init(void)
{
    mux_port_t *p = &g_mux.c6;

    char why[RADIO_MUX_REASON_MAX];

    p->chan = &g_mux.chans[0];
    c6_list(c6_probe_present(why, sizeof(why)), why); // no probe yet: listed (its steps decide)
    p->busy = true;
    p->work = RADIO_MUX_WORK_BOOT_STEPS;
    p->att.state = RADIO_MUX_AS_ABSENT;
    radio_mux_reinit_ask(&g_mux.reinit, 0, false, now_ms()); // nothing is bound yet: it runs at once
    atomic_store(&g_mux.maint_running, true);
    refresh(p);
    ESP_LOGI(TAG, "%s: reinit: %s", p->chan->id, radio_mux_work_reason(p->work));
}

// The C6 enters reinit for its maintenance: the bound instance let go (or none is bound). Its transport is released
// (SPI2, INT and the reset GPIO; the C6 left running) for the built-in RCP's ops. What the instance still sends is
// refused (tx: ESP_ERR_INVALID_STATE, nothing written; a reset is not carried out, nothing being attached) and nothing
// reaches it (no frames, no RESET). Its binding stays: the end of the reinit binds it again (BACK).
static void maint_begin(mux_port_t *p, radio_mux_work_t work)
{
    if (p->att.state == RADIO_MUX_AS_ATTACHED) {
        atomic_store(&p->tx_ok, false);
        mux_set_rx(p, MUX_RX_DISCARD);
        xSemaphoreTake(p->rx_lock, portMAX_DELAY);
        p->bound = NULL;
        xSemaphoreGive(p->rx_lock);
        p->inst = NULL;
    }
    c6_transport_release();
    p->busy = true;
    p->work = work;
    p->att.state = RADIO_MUX_AS_ABSENT;
    refresh(p);
    ESP_LOGI(TAG, "%s: reinit: %s", p->chan->id, radio_mux_work_reason(work));
}

static void handle_maint_done(mux_call_t *call);

void mux_maint_answer(radio_mux_reinit_act_t act, esp_err_t verdict, const char *reason)
{
    mux_call_t *call = g_mux.maint_call;

    if (act != RADIO_MUX_RA_RUN && act != RADIO_MUX_RA_REFUSE) {
        return;
    }
    g_mux.maint_call = NULL;
    if (act == RADIO_MUX_RA_REFUSE) {
        atomic_store(&g_mux.maint_running, false);
        ESP_LOGW(TAG, "%s: reinit (%s) refused: %s", g_mux.c6.chan->id, radio_mux_work_reason(g_mux.maint_work),
                 reason && reason[0] ? reason : esp_err_to_name(verdict));
        if (call) {
            call->err = verdict == ESP_OK ? ESP_ERR_INVALID_STATE : verdict;
            snprintf(call->reason, sizeof(call->reason), "%s", reason ? reason : "");
        }
    } else {
        int pending = MUX_CALL_PENDING;

        maint_begin(&g_mux.c6, g_mux.maint_work);
        if (call && !atomic_compare_exchange_strong(&call->claim, &pending, MUX_CALL_TAKEN)) {
            // The caller gave up waiting: no work runs. The reinit ends at once (the instance gets its channel back).
            ESP_LOGW(TAG, "%s: maintenance given up by its caller: the reinit ends", g_mux.c6.chan->id);
            mux_call_t none = {.steps = ESP_OK}; // no work, no steps: only the end (handle_maint_done reads no more)

            handle_maint_done(&none);
        } else if (call) {
            call->err = ESP_OK;
        }
    }
    if (call) {
        xSemaphoreGive(call->done);
        mux_call_put(call);
    }
}

// MAINT_ASK: refused while another maintenance runs or OpenThread is being moved to the C6; otherwise the instance
// bound to it is asked to let go (REINIT), or, with none bound, the C6 enters reinit at once.
static void handle_maint_ask(mux_call_t *call)
{
    mux_port_t *p = &g_mux.c6;
    const struct radio_mux_chan *ch = find_chan(call->id);
    const char *why = NULL;

    call->err = ESP_ERR_INVALID_STATE;
    if (!ch || ch != p->chan) {
        call->err = ESP_ERR_NOT_FOUND;
        why = "not the built-in RCP's channel";
    } else if (c6_maintained(p)) {
        why = p->work == RADIO_MUX_WORK_BOOT_STEPS ? "the radio is still booting" :
              "a firmware push or ROM recovery is already running";
    } else if (p->leased) {
        why = "OpenThread is being moved to it";
    }
    if (why) {
        snprintf(call->reason, sizeof(call->reason), "%s", why);
        xSemaphoreGive(call->done);
        mux_call_put(call);
        return;
    }
    orch_inst_t *oi = p->att.state == RADIO_MUX_AS_ATTACHED ? p->inst : NULL;

    g_mux.maint_call = call; // the owner's reference: answered by mux_maint_answer
    g_mux.maint_work = call->work;
    g_mux.maint_inst = oi;
    atomic_store(&g_mux.maint_running, true);
    const radio_mux_reinit_act_t act = orch_reinit_ask(p, oi, call->work, now_ms());

    if (act == RADIO_MUX_RA_RUN) {
        orch_reinit_notice(p, call->work); // an instance stored on it but not bound (the C6 in fault) hears of it
    }
    mux_maint_answer(act, ESP_OK, NULL);
}

// Owner loop: an instance asked to let go that did not answer by its deadline refused.
static void maint_tick(void)
{
    char why[RADIO_MUX_REASON_MAX];

    if (radio_mux_reinit_tick(&g_mux.reinit, now_ms()) != RADIO_MUX_RA_REFUSE) {
        return;
    }
    snprintf(why, sizeof(why), "%s did not let go of the radio in time",
             g_mux.maint_inst ? radio_mux_mode_name(g_mux.maint_inst->inst.mode) : "its mode");
    mux_maint_answer(RADIO_MUX_RA_REFUSE, ESP_ERR_INVALID_STATE, why);
}

// MAINT_DONE: the work and the boot steps ended. The C6 is listed from its last probe (unavailable when it does not
// work, or when the steps failed); usable, its stored mode's instance is bound again (BACK, or BIND the first time);
// otherwise that instance gets LOST with the steps' reason.
static void handle_maint_done(mux_call_t *call)
{
    mux_port_t *p = &g_mux.c6;
    const radio_mux_work_t work = p->work;
    char why[RADIO_MUX_REASON_MAX];

    call->err = ESP_OK;
    if (!p->busy) {
        return; // no reinit runs
    }
    p->busy = false;
    p->work = RADIO_MUX_WORK_NONE;
    radio_mux_reinit_done(&g_mux.reinit);
    atomic_store(&g_mux.maint_running, false);
    bool present = c6_probe_present(why, sizeof(why));

    if (present && call->steps != ESP_OK) {
        present = false;
        snprintf(why, sizeof(why), "%s", C6_JOB_FAILED_REASON);
    }
    c6_list(present, why);
    g_mux.maint_ending = true;
    c6_apply(false);
    g_mux.maint_ending = false;
    if (!present) {
        orch_lost(p->chan, p->att_handle, RADIO_MUX_FAULT_FIRMWARE, call->reason[0] ? call->reason : why);
    } else if (p->att.state != RADIO_MUX_AS_ATTACHED && p->fault[0] != '\0' && p->fault_kind != RADIO_MUX_FAULT_NONE) {
        // Listed usable, but the C6 did not open again (a link fault): the instance that let go must hear of it.
        orch_lost(p->chan, p->att_handle, p->fault_kind, p->fault);
    }
    ESP_LOGI(TAG, "%s: reinit (%s) ended: %s", p->chan->id, radio_mux_work_reason(work), present ? "ready" : why);
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
    } else if (p->kind == RADIO_MUX_KIND_BUILTIN && c6_maintained(p)) {
        why = p->busy ? c6_busy_reason(p) : "maintenance waits for its instance to let go";
    } else if (p->leased) {
        why = "OpenThread is already being moved to it";
    } else if (p->att.state == RADIO_MUX_AS_ATTACHED || p->inst) {
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
    if (p->kind == RADIO_MUX_KIND_BUILTIN && c6_transport_open(msg, sizeof(msg)) != ESP_OK) {
        call->err = ESP_FAIL;
        snprintf(call->reason, sizeof(call->reason), "%s", msg);
        return;
    }
    radio_mux_chan_t *h = mux_new_attach(p);

    p->inst = NULL;
    p->tx_frames = true;
    mux_set_rx(p, MUX_RX_FRAMES);
    atomic_store(&p->tx_ok, true);
    p->leased = true;
    p->fault[0] = '\0';
    xSemaphoreTake(p->rx_lock, portMAX_DELAY);
    radio_mux_reset_count_clear(&p->reset_count); // a lease is an attach: no unanswered reset carries over
    xSemaphoreGive(p->rx_lock);
    refresh(p);
    call->handle = h;
    call->err = ESP_OK;
    ESP_LOGI(TAG, "%s: leased for OTBR", ch->id);
}

// Gives a leased channel back: delivery stops, the C6 returns to what its stored mode wants (off: reset). A fault
// text stays as the channel's reason (state fault) until its next mode change, plug-in or maintenance.
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
    if (p->kind == RADIO_MUX_KIND_BUILTIN) {
        c6_apply(false);
    }
    snprintf(p->fault, sizeof(p->fault), "%s", call->reason);
    p->fault_kind = RADIO_MUX_FAULT_NONE; // OpenThread's, not the channel's
    refresh(p);
    ESP_LOGI(TAG, "%s: lease ended%s%s", ch->id, call->reason[0] ? ": " : "", call->reason);
}

// The commit of a move: `to` (leased, OpenThread starts on it next) is stored in otbr and `from` in off, in that order
// of effect: Thread is unbound from `from` (GONE), then the lease becomes its binding on `to` (no GONE, no reopen).
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

    if (p->kind == RADIO_MUX_KIND_BUILTIN) {
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
    orch_inst_t *oi = orch_find(RADIO_MUX_MODE_OTBR);

    if (!to || !to->port->leased || to->port->chan != to || !oi) {
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
    // Both in one NVS handle and commit; should a reset still cut them apart, the load keeps the first otbr channel.
    call->err = mux_store_chans(to, from);
    if (call->err != ESP_OK) {
        mux_lock();
        to->mode = to_old;
        if (from && from != to) {
            from->mode = from_old;
        }
        mux_unlock();
        mux_store_chans(to, from); // best effort: back to what was stored
        return;
    }
    if (from && from != to) {
        mux_port_t *fp = from->port;

        if (fp->kind == RADIO_MUX_KIND_BUILTIN) {
            c6_apply(false); // otbr detached (GONE), the C6 held in reset (off)
        } else if (fp->chan == from) {
            feed(fp, RADIO_MUX_AEV_MODE_SET);
        }
        orch_release(from, RADIO_MUX_MODE_OTBR); // an absent source: the binding goes all the same
    }
    mux_port_t *p = to->port;

    p->leased = false;
    p->inst = oi;
    p->att.state = RADIO_MUX_AS_ATTACHED;
    orch_bind(p, oi, p->att_handle); // the lease's handle: the same generation goes on
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
        ch = evict_slot(id, true);
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
    // Not on a port, so no attach state sets its lifecycle: a link fault with the reason while it is plugged in.
    ch->chan_state = RADIO_MUX_CS_FAULT;
    ch->fault_kind = RADIO_MUX_FAULT_LINK;
    ch->work = RADIO_MUX_WORK_NONE;
    snprintf(ch->chan_reason, sizeof(ch->chan_reason), "%s", ch->reason);
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
            ch->chan_state = RADIO_MUX_CS_ABSENT;
            ch->fault_kind = RADIO_MUX_FAULT_NONE;
            ch->chan_reason[0] = '\0';
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
            ch->chan_state = RADIO_MUX_CS_ABSENT;
            ch->fault_kind = RADIO_MUX_FAULT_NONE;
            ch->chan_reason[0] = '\0';
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
    if (ch->kind == RADIO_MUX_KIND_BUILTIN && (req->has_baud || req->has_flow)) {
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
    // The C6 in reinit (or asked to enter it) refuses every mode change until it is ready again.
    const bool in_reinit = ch->port->kind == RADIO_MUX_KIND_BUILTIN && c6_maintained(ch->port);
    const radio_mux_verdict_t v = radio_mux_orch_check_bind(&g_mux.rules, views, n, target, want, in_reinit,
                                                            &move_from);

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
    if (p->kind == RADIO_MUX_KIND_BUILTIN) {
        // As on USB: a new mode or new rcp_proxy settings (in rcp-proxy) re-attach, a PUT of the mode retries a fault.
        // While the C6 is unavailable the change is stored and applies once it is listed again (in reinit a mode change
        // is refused: check_mode).
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
    if (ch->kind == RADIO_MUX_KIND_BUILTIN || ch->present || ch->port->chan == ch) {
        call->err = ESP_ERR_INVALID_STATE;
        return;
    }
    call->err = mux_erase_chan(ch);
    if (call->err == ESP_OK) {
        mux_lock();
        ch->used = false;
        mux_unlock();
        orch_forget_slot((size_t)(ch - g_mux.chans));
        ESP_LOGI(TAG, "%s forgotten", call->id);
    }
}

// A leased channel whose device did not come back from a reopen: the lease ends as an unplug ends it (GONE to the
// lease holder, which then fails its start and gives the lease back); the channel shows p->fault.
static void lease_lost(mux_port_t *p)
{
    p->leased = false;
    atomic_store(&p->tx_ok, false);
    mux_set_rx(p, MUX_RX_DISCARD);
    mux_post_ctl(p, RADIO_MUX_EV_GONE);
    refresh(p);
}

// The USB part of a reset (a consumer's or a lease holder's request, or a transfer error that stopped the device's
// polling): close, reopen, RESET to whoever holds the channel. GONE (detach, or the end of a lease) when it does not
// reopen, or when this is the RADIO_MUX_USB_DEAD_RESETS-th reset in a row with nothing received in between (closed,
// fault "no answer after 3 resets"; an unplug or a PUT of the mode opens it again).
static void usb_reset(mux_port_t *p, const char *what)
{
    struct radio_mux_chan *ch = p->chan;

    xSemaphoreTake(p->rx_lock, portMAX_DELAY);
    const bool give_up = radio_mux_reset_count_request(&p->reset_count);
    xSemaphoreGive(p->rx_lock);

    atomic_store(&p->tx_ok, false);
    mux_set_rx(p, MUX_RX_DISCARD);
    usb_transport_close(p);
    if (give_up) {
        snprintf(p->fault, sizeof(p->fault), "%s", RADIO_MUX_USB_DEAD_REASON);
        p->fault_kind = RADIO_MUX_FAULT_LINK;
        ESP_LOGW(TAG, "%s: %s: closed (unplug it or PUT its mode to open it again)", ch->id, p->fault);
    }
    if (give_up || !open_dev(p, mux_eff_baud(ch))) {
        if (p->leased) {
            lease_lost(p);
        } else {
            feed(p, RADIO_MUX_AEV_MODE_SET); // not open: the machine detaches the consumer (GONE), shows the fault
        }
        return;
    }
    taskENTER_CRITICAL(&p->lock);
    p->ctr.resets++;
    taskEXIT_CRITICAL(&p->lock);
    mux_set_rx(p, p->tx_frames ? MUX_RX_FRAMES : MUX_RX_RAW);
    atomic_store(&p->tx_ok, true);
    mux_post_ctl(p, RADIO_MUX_EV_RESET); // to the consumer's queue, or the lease's (where its frames go)
    ESP_LOGI(TAG, "%s: reopened (%s)", ch->id, what);
}

// A reset through a consumer's or a lease holder's handle (radio_mux_reset_applies): a dongle is closed and reopened
// over USB (usb_reset), the C6 gets its reset GPIO pulse.
static void handle_reset(radio_mux_chan_t *h)
{
    mux_port_t *p;
    uint32_t gen;
    struct radio_mux_chan *ch = mux_handle_chan(h, &p, &gen);

    if (!ch || !radio_mux_reset_applies(p->att.state, p->leased, p->chan == ch,
                                        radio_mux_handle_current(gen, atomic_load(&p->att_gen)))) {
        return; // a channel that is not attached or leased has nobody to tell; an earlier attach's reset is not ours
    }
    if (p->kind == RADIO_MUX_KIND_BUILTIN) { // the reset GPIO pulse; the C6 task posts RESET when the RCP answers
        if (c6_transport_reset() == ESP_OK) {
            taskENTER_CRITICAL(&p->lock);
            p->ctr.resets++;
            taskEXIT_CRITICAL(&p->lock);
            ESP_LOGI(TAG, "%s: reset", ch->id);
        }
        return;
    }
    usb_reset(p, "reset");
}

// Owner loop: a CDC_ACM_HOST_ERROR flagged on the open device (its polling stopped) reopens it, at most once per
// RADIO_MUX_USB_ERROR_REOPEN_US (a later flag waits for the loop's next pass). Attached or leased: as a reset (RESET
// to its holder, counted by the dead-reset rule, which a frame received afterwards clears). Open with nobody on it:
// closed and reopened. During the detection and its reopen: nothing (that reopen follows anyway).
static void usb_error_check(mux_port_t *p)
{
    const int64_t now = esp_timer_get_time();

    if (!radio_mux_usb_error_due(atomic_load(&p->usb_error), p->usb_error_us, now)) {
        return;
    }
    atomic_store(&p->usb_error, false);
    if (!usb_transport_is_open() || !p->chan || p->att.state == RADIO_MUX_AS_PROBING ||
            p->att.state == RADIO_MUX_AS_REOPENING) {
        return;
    }
    p->usb_error_us = now;
    ESP_LOGW(TAG, "%s: a USB transfer error stopped the device's polling: reopening", p->chan->id);
    if (p->att.state == RADIO_MUX_AS_ATTACHED || p->leased) {
        usb_reset(p, "USB transfer error");
        return;
    }
    mux_set_rx(p, MUX_RX_DISCARD);
    usb_transport_close(p);
    open_dev(p, mux_eff_baud(p->chan));
    refresh(p);
}

// Owner loop: a USB close that failed is retried; once it closes, a channel that could not open because of it opens
// (as a PUT of its mode would); when the retries are spent, its channel shows the fault.
static void usb_close_check(mux_port_t *p)
{
    const radio_mux_close_act_t act = usb_transport_close_retry();

    if (act == RADIO_MUX_CLOSE_GAVE_UP && p->chan && !usb_transport_is_open()) {
        snprintf(p->fault, sizeof(p->fault), "%s", RADIO_MUX_USB_CLOSE_FAILED_REASON);
        p->fault_kind = RADIO_MUX_FAULT_LINK;
        if (p->att.state == RADIO_MUX_AS_ATTACHED) {
            feed(p, RADIO_MUX_AEV_MODE_SET); // not open: detached (GONE)
        }
        refresh(p);
        return;
    }
    if (act != RADIO_MUX_CLOSE_DONE || !p->chan || usb_transport_is_open() || p->att.state != RADIO_MUX_AS_IDLE ||
            p->leased || strcmp(p->fault, RADIO_MUX_USB_CLOSE_PENDING_REASON) != 0) {
        return;
    }
    if (open_dev(p, mux_eff_baud(p->chan))) {
        feed(p, RADIO_MUX_AEV_MODE_SET); // open again: the stored mode's consumer attaches
    }
    refresh(p);
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
    if (ch->kind == RADIO_MUX_KIND_BUILTIN || ch->remembered) {
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
    case MUX_MSG_USB_ERROR: // a wakeup: the flag says what happened
        usb_error_check(&g_mux.usb);
        break;
    case MUX_MSG_REINIT_REPLY: { // a wakeup: the answer is in the mailbox
        mux_reply_t r;

        while (mux_take_reply(&r)) {
            orch_reinit_reply(r.h, r.seq, r.verdict, r.reason);
        }
        break;
    }
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
#ifdef OTBR_TEST_BUILD
    case MUX_MSG_TEST_REOPEN: { // handle_reset's USB path for whoever holds the dongle, without a handle
        mux_port_t *p = &g_mux.usb;

        if (p->chan && usb_transport_is_open() && radio_mux_reset_applies(p->att.state, p->leased, true, true)) {
            // A deliberate reopen from the console is not an unanswered reset: it must not trip the dead-reset rule
            // (tests reopen back to back under traffic).
            radio_mux_reset_count_clear(&p->reset_count);
            usb_reset(p, "test usb_reopen"); // a failed reopen detaches (GONE) or ends the lease, as a reset's does
        } else {
            ESP_LOGW(TAG, "test: usb_reopen ignored: no dongle attached or leased");
        }
        break;
    }
#endif
    case MUX_MSG_NOTE_FW:
        handle_note_fw(m->note.h, &m->note.fw);
        break;
    case MUX_MSG_MAINT_ASK:
        handle_maint_ask(m->call); // answers now or once the instance did (mux_maint_answer)
        break;
    case MUX_MSG_MAINT_DONE:
        handle_maint_done(m->call);
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
    // R13 at start: every present channel whose stored mode has an instance: the C6 once its boot steps ended (it starts
    // in reinit), USB devices attached at boot through new_dev_cb like any plug-in.
    c6_init();
    for (;;) {
        const bool probing = g_mux.usb.att.state == RADIO_MUX_AS_PROBING;
        const bool usb_due = usb_transport_close_pending() || atomic_load(&g_mux.usb.usb_error);
        const int wait_ms = probing ? DETECT_POLL_MS : usb_due ? USB_DUE_POLL_MS : IDLE_POLL_MS;

        if (xQueueReceive(g_mux.queue, &m, WAIT_TICKS(wait_ms)) == pdTRUE) {
            handle(&m);
            while (xQueueReceive(g_mux.queue, &m, 0) == pdTRUE) {
                handle(&m);
            }
        }
        if (g_mux.usb.att.state == RADIO_MUX_AS_PROBING) {
            detect_step(&g_mux.usb);
        }
        check_presence(&g_mux.usb);
        usb_error_check(&g_mux.usb);
        usb_close_check(&g_mux.usb);
        mux_reply_t r;

        while (mux_take_reply(&r)) { // answers whose wakeup did not fit into the queue: before the deadline is checked
            orch_reinit_reply(r.h, r.seq, r.verdict, r.reason);
        }
        maint_tick();
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

// The built-in RCP's boot steps, on a task of their own (they run even when the network does not come up, and cost no
// RAM once done): the C6 is in reinit from the start until they end. Deletes itself.
static void boot_task(void *arg)
{
    mux_maint_finish(RADIO_MUX_BUILTIN_ID);
    vTaskDelete(NULL);
}

esp_err_t radio_mux_start(void)
{
    ESP_RETURN_ON_FALSE(!atomic_load(&g_mux.started), ESP_ERR_INVALID_STATE, TAG, "already started");
    g_mux.lock = xSemaphoreCreateMutex();
    g_mux.queue = xQueueCreate(QUEUE_LEN, sizeof(mux_msg_t));
    ESP_RETURN_ON_FALSE(g_mux.lock && g_mux.queue, ESP_ERR_NO_MEM, TAG, "queue and lock");
    ESP_RETURN_ON_ERROR(port_init(&g_mux.c6, RADIO_MUX_KIND_BUILTIN), TAG, "c6 port");
    ESP_RETURN_ON_ERROR(port_init(&g_mux.usb, RADIO_MUX_KIND_USB), TAG, "USB port");
    mux_load_chans();
    ESP_RETURN_ON_ERROR(c6_transport_start(), TAG, "c6 task");
    atomic_store(&g_mux.started, true);
    if (xTaskCreatePinnedToCore(owner_task, "radio_mux", OWNER_STACK, NULL, OWNER_PRIORITY, NULL, MGMT_CORE) !=
            pdPASS) {
        atomic_store(&g_mux.started, false);
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreatePinnedToCore(boot_task, "mux_boot", BOOT_TASK_STACK, NULL, BOOT_TASK_PRIORITY, NULL, MGMT_CORE) !=
            pdPASS) {
        // Without its boot steps the C6 is still used: the reinit ends as if they passed (its installed firmware is
        // tried, as when the probe could not run).
        ESP_LOGE(TAG, "the boot steps' task could not be created: the C6 is used without them");
        mux_call_t *call = calloc(1, sizeof(*call));

        if (call) {
            call->done = xSemaphoreCreateBinary();
            atomic_store(&call->refs, 1); // the owner's only
            snprintf(call->id, sizeof(call->id), "%s", RADIO_MUX_BUILTIN_ID);
            const mux_msg_t m = {.type = MUX_MSG_MAINT_DONE, .call = call};

            if (!call->done || xQueueSend(g_mux.queue, &m, portMAX_DELAY) != pdTRUE) {
                ESP_LOGE(TAG, "the C6 stays in reinit");
            }
        }
    }
    size_t n = 0;

    for (size_t i = 0; i < RADIO_MUX_CHANNELS_MAX; i++) {
        n += g_mux.chans[i].used;
    }
    ESP_LOGI(TAG, "started: %u channels known, %u mode instances", (unsigned)n, (unsigned)g_mux.n_insts);
    return ESP_OK;
}

bool mux_post(const mux_msg_t *m)
{
    return g_mux.queue && xQueueSend(g_mux.queue, m, 0) == pdTRUE;
}
#ifdef OTBR_TEST_BUILD
esp_err_t radio_mux_test_usb_reopen(void)
{
    const mux_msg_t m = {.type = MUX_MSG_TEST_REOPEN};

    return mux_post(&m) ? ESP_OK : ESP_ERR_TIMEOUT;
}
#endif
