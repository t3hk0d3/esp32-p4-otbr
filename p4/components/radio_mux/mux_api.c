// The consumer side (bind, poll, release, tx, reset, raw reads, firmware notes) and the API side (snapshot, set,
// forget, otbr channel, mover) of the mux. Calls that change a channel post a message and wait for the owner task.
#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "radio_mux_priv.h"

static const char *TAG = "radio_mux";

#define CALL_WAIT_MS 5000
#define RESET_POST_WAIT_MS 1000 // a reset must not be lost to a queue that is full for a moment
#define END_ALLOC_RETRY_MS 100

/* ---- consumer side ---- */

// The transport behind handle `h` when `h` is of its current attach (the record in *chp when chp is not NULL); NULL
// otherwise. Callers that take something check again under the lock that guards it.
static mux_port_t *current_port(const radio_mux_chan_t *h, struct radio_mux_chan **chp)
{
    mux_port_t *p;
    uint32_t gen;
    struct radio_mux_chan *ch = atomic_load(&g_mux.started) ? mux_handle_chan(h, &p, &gen) : NULL;

    if (!ch || !radio_mux_handle_current(gen, atomic_load(&p->att_gen))) {
        return NULL;
    }
    if (chp) {
        *chp = ch;
    }
    return p;
}

// Whether `h` is of p's current attach (p from mux_handle_chan or current_port).
static bool is_current(const mux_port_t *p, const radio_mux_chan_t *h)
{
    size_t slot;
    uint32_t gen;

    return radio_mux_handle_unpack((uintptr_t)h, &slot, &gen) &&
           radio_mux_handle_current(gen, atomic_load(&p->att_gen));
}

esp_err_t radio_mux_bind_queue(radio_mux_chan_t *ch, QueueHandle_t q)
{
    struct radio_mux_chan *rec = NULL;
    mux_port_t *p = current_port(ch, &rec);
    radio_mux_ev_t ev;
    esp_err_t err = ESP_OK;

    ESP_RETURN_ON_FALSE(p, ESP_ERR_INVALID_STATE, TAG, "not attached (or a handle of an earlier attach)");
    // rx_lock: no delivery while the queue changes; ev_lock: the generation cannot move on meanwhile (the owner unbinds
    // under rx_lock after it moved the generation on, so a late bind of an earlier handle never sticks).
    xSemaphoreTake(p->rx_lock, portMAX_DELAY);
    xSemaphoreTake(p->ev_lock, portMAX_DELAY);
    if (!is_current(p, ch)) {
        err = ESP_ERR_INVALID_STATE;
    } else {
        p->bound = q;
        // What arrived between the start of the receive path and this call moves over, in order.
        while (q && xQueueReceive(p->evq, &ev, 0) == pdTRUE) {
            if (xQueueSend(q, &ev, 0) != pdTRUE && ev.type == RADIO_MUX_EV_RX_FRAME) {
                mux_slot_put(p, ev.slot);
                taskENTER_CRITICAL(&p->lock);
                p->ctr.drops++;
                taskEXIT_CRITICAL(&p->lock);
            }
        }
    }
    xSemaphoreGive(p->ev_lock);
    xSemaphoreGive(p->rx_lock);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s: bind through a handle of an earlier attach: refused", rec->id);
    }
    return err;
}

int radio_mux_eventfd(radio_mux_chan_t *ch)
{
    mux_port_t *p;
    uint32_t gen;

    return atomic_load(&g_mux.started) && mux_handle_chan(ch, &p, &gen) ? p->efd : -1;
}

bool radio_mux_poll(radio_mux_chan_t *ch, radio_mux_ev_t *ev)
{
    mux_port_t *p = current_port(ch, NULL);
    bool got = false;

    if (!p) {
        return false; // a handle of an earlier attach: the queue is the next consumer's
    }
    xSemaphoreTake(p->ev_lock, portMAX_DELAY);
    if (is_current(p, ch)) {
        got = xQueueReceive(p->evq, ev, 0) == pdTRUE;
        if (!got) {
            mux_rearm(p);
        }
    }
    xSemaphoreGive(p->ev_lock);
    return got;
}

void radio_mux_release(radio_mux_chan_t *ch, uint8_t *slot)
{
    mux_port_t *p;
    uint32_t gen;

    // Any generation: the slot belongs to the transport's pool, whoever was attached when it was delivered.
    if (slot && atomic_load(&g_mux.started) && mux_handle_chan(ch, &p, &gen)) {
        mux_slot_put(p, slot);
    }
}

esp_err_t radio_mux_tx(radio_mux_chan_t *ch, const uint8_t *data, size_t n)
{
    mux_port_t *p = current_port(ch, NULL);

    if (!p || !atomic_load(&p->tx_ok)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!data || n == 0 || (p->tx_frames && n > RADIO_MUX_FRAME_MAX)) {
        return n == 0 ? ESP_ERR_INVALID_ARG : ESP_ERR_INVALID_SIZE;
    }
    esp_err_t err;

    // tx_lock: the owner moves the generation on only with it held, so the check below holds for the whole send.
    xSemaphoreTake(p->tx_lock, portMAX_DELAY);
    if (!atomic_load(&p->tx_ok) || !is_current(p, ch)) {
        err = ESP_ERR_INVALID_STATE;
    } else if (p->kind == RADIO_MUX_KIND_C6) {
        err = radio_mux_tx_transport_err(c6_transport_tx(data, n)); // one SPI transaction (frames only, R23)
    } else if (p->tx_frames) {
        const size_t len = radio_mux_hdlc_encode(data, n, p->tx_buf, p->tx_cap);

        err = len ? radio_mux_tx_transport_err(usb_transport_write(p->tx_buf, len)) : ESP_ERR_INVALID_SIZE;
    } else {
        err = radio_mux_tx_transport_err(usb_transport_write(data, n));
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
    mux_port_t *p;
    uint32_t gen;

    ESP_RETURN_ON_FALSE(atomic_load(&g_mux.started) && mux_handle_chan(ch, &p, &gen), ESP_ERR_INVALID_STATE, TAG,
                        "not started");
    const mux_msg_t m = {.type = MUX_MSG_RESET, .h = ch}; // the owner checks the generation

    if (xQueueSend(g_mux.queue, &m, pdMS_TO_TICKS(RESET_POST_WAIT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "reset not posted: the owner task's queue stayed full for %d ms", RESET_POST_WAIT_MS);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

size_t radio_mux_read_raw(radio_mux_chan_t *ch, uint8_t *out, size_t cap)
{
    mux_port_t *p = current_port(ch, NULL);
    size_t n = 0;

    if (!p || !p->ring) {
        return 0; // a handle of an earlier attach: the ring is the next consumer's
    }
    xSemaphoreTake(p->ev_lock, portMAX_DELAY);
    if (is_current(p, ch)) {
        n = mux_ring_take(p, out, cap);
        if (n == 0) {
            mux_rearm(p);
        }
    }
    xSemaphoreGive(p->ev_lock);
    return n;
}

bool radio_mux_chan_frames(const radio_mux_chan_t *ch)
{
    mux_port_t *p = current_port(ch, NULL);

    return p && (p->kind == RADIO_MUX_KIND_C6 || p->tx_frames);
}

const char *radio_mux_chan_id(const radio_mux_chan_t *ch)
{
    mux_port_t *p;
    uint32_t gen;
    const struct radio_mux_chan *rec = mux_handle_chan(ch, &p, &gen);

    return rec ? rec->id : NULL;
}

void radio_mux_get_proxy(const radio_mux_chan_t *ch, radio_mux_proxy_settings_t *out)
{
    mux_port_t *p;
    uint32_t gen;
    const struct radio_mux_chan *rec = g_mux.lock ? mux_handle_chan(ch, &p, &gen) : NULL;

    if (!rec) {
        radio_mux_proxy_defaults(out);
        return;
    }
    mux_lock();
    *out = rec->proxy;
    mux_unlock();
}

void radio_mux_report(radio_mux_chan_t *ch, const char *state, const char *reason)
{
    struct radio_mux_chan *rec;
    mux_port_t *p = current_port(ch, &rec);

    if (!p || !state) {
        return;
    }
    mux_lock();
    if (is_current(p, ch)) {
        snprintf(rec->mode_state, sizeof(rec->mode_state), "%s", state);
        snprintf(rec->mode_reason, sizeof(rec->mode_reason), "%s", reason ? reason : "");
    }
    mux_unlock();
}

esp_err_t radio_mux_reinit_reply(radio_mux_chan_t *ch, uint32_t seq, esp_err_t verdict, const char *reason)
{
    mux_msg_t m = {.type = MUX_MSG_REINIT_REPLY};

    m.reply.h = ch; // the owner checks the generation and the seq
    m.reply.seq = seq;
    m.reply.verdict = verdict;
    snprintf(m.reply.reason, sizeof(m.reply.reason), "%s", reason ? reason : "");
    if (!mux_post(&m)) {
        ESP_LOGW(TAG, "reinit answer %" PRIu32 " lost: queue full", seq);
        return ESP_FAIL;
    }
    return ESP_OK;
}

void radio_mux_note_firmware(radio_mux_chan_t *ch, const radio_mux_fw_record_t *fw)
{
    mux_port_t *p;
    uint32_t gen;
    const struct radio_mux_chan *rec = fw && atomic_load(&g_mux.started) ? mux_handle_chan(ch, &p, &gen) : NULL;

    if (!rec) {
        return;
    }
    mux_msg_t m = {.type = MUX_MSG_NOTE_FW};

    m.note.h = ch; // the owner checks the generation
    m.note.fw = *fw;
    if (!mux_post(&m)) {
        ESP_LOGW(TAG, "%s: firmware note lost: queue full", rec->id);
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
    o->line.baud = mux_eff_baud(ch);
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
    o->chan_state = ch->chan_state;
    o->fault_kind = ch->fault_kind;
    o->work = ch->work;
    memcpy(o->chan_reason, ch->chan_reason, sizeof(o->chan_reason));
    memcpy(o->mode_state, ch->mode_state, sizeof(o->mode_state));
    memcpy(o->mode_reason, ch->mode_reason, sizeof(o->mode_reason));
}

void radio_mux_get_snapshot(radio_mux_snapshot_t *out)
{
    out->n = 0;
    out->n_modes = 0;
    out->available_modes = RADIO_MUX_MODE_BIT(RADIO_MUX_MODE_OFF);
    out->reboot_modes = 0;
    if (!atomic_load(&g_mux.started)) {
        return;
    }
    mux_lock();
    out->available_modes = radio_mux_available_modes(&g_mux.rules);
    out->reboot_modes = radio_mux_reboot_modes(&g_mux.rules);
    for (size_t i = 0; i < RADIO_MUX_CHANNELS_MAX; i++) {
        if (g_mux.chans[i].used) {
            fill_info(&g_mux.chans[i], &out->chans[out->n++]);
        }
    }
    orch_fill_modes(out);
    mux_unlock();
}

// Posts a call to the owner and waits for it. ESP_ERR_TIMEOUT when it could not be posted in time: the owner never
// sees it. ESP_ERR_NOT_FINISHED when it was posted but not answered in time: the owner still runs it when it gets to
// it (unless the caller abandons a claimable call) and frees the record then.
static esp_err_t call_owner_ms(mux_msg_type_t type, mux_call_t *call, uint32_t answer_ms)
{
    const mux_msg_t m = {.type = type, .call = call};

    if (xQueueSend(g_mux.queue, &m, pdMS_TO_TICKS(CALL_WAIT_MS)) != pdTRUE) {
        mux_call_put(call); // the owner's reference
        ESP_LOGW(TAG, "%s: the owner task's queue stayed full for %d ms: not posted", call->id, CALL_WAIT_MS);
        return ESP_ERR_TIMEOUT;
    }
    if (xSemaphoreTake(call->done, pdMS_TO_TICKS(answer_ms)) != pdTRUE) {
        ESP_LOGW(TAG, "%s: the owner task did not answer in %" PRIu32 " ms", call->id, answer_ms);
        return ESP_ERR_NOT_FINISHED;
    }
    return ESP_OK;
}

static esp_err_t call_owner(mux_msg_type_t type, mux_call_t *call)
{
    return call_owner_ms(type, call, CALL_WAIT_MS);
}

// An end message (a reinit's end, a lease's end) must reach the owner, or c6 stays in reinit or the channel leased until
// a reboot: its record is allocated until it can be (call_new_end), the post waits as long as it takes, and only the
// answer may time out (logged; the owner runs it later).
static void call_owner_end(mux_msg_type_t type, mux_call_t *call, const char *what)
{
    const mux_msg_t m = {.type = type, .call = call};

    xQueueSend(g_mux.queue, &m, portMAX_DELAY);
    if (xSemaphoreTake(call->done, pdMS_TO_TICKS(CALL_WAIT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "%s: %s: the owner task did not answer in %d ms; it runs when the owner gets to it", call->id,
                 what, CALL_WAIT_MS);
    }
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

static mux_call_t *call_new_end(const char *id)
{
    mux_call_t *call = call_new(id);

    if (!call) {
        ESP_LOGE(TAG, "%s: no memory for an end message: trying again until there is", id);
    }
    while (!call) {
        vTaskDelay(pdMS_TO_TICKS(END_ALLOC_RETRY_MS));
        call = call_new(id);
    }
    return call;
}

radio_mux_verdict_t radio_mux_set(const char *id, const radio_mux_set_req_t *req, char *moved_from, size_t cap,
                                  char *reason, size_t reason_cap)
{
    if (reason && reason_cap > 0) {
        reason[0] = '\0';
    }
    if (!id || !req || !atomic_load(&g_mux.started)) {
        return RADIO_MUX_REFUSE_FAILED;
    }
    esp_err_t (*mover)(const char *, const char *, char *, size_t) = atomic_load(&g_mux.mover);
    mux_call_t *call = call_new(id);

    if (!call) {
        return RADIO_MUX_REFUSE_FAILED;
    }
    call->req = *req;
    call->req.kick = false; // never stored; device_api kicks through rcp_proxy (R6)
    call->have_mover = mover != NULL;
    const esp_err_t posted = call_owner(MUX_MSG_SET, call);

    if (posted != ESP_OK) {
        mux_call_put(call);
        // Not posted: nothing happens. Posted but not answered: the owner still applies it later (a MOVE it finds
        // then does nothing: the mover is never called).
        return posted == ESP_ERR_NOT_FINISHED ? RADIO_MUX_REFUSE_PENDING : RADIO_MUX_REFUSE_FAILED;
    }
    radio_mux_verdict_t v = call->verdict;
    char from[RADIO_MUX_ID_MAX];
    const bool stop = call->stop;

    snprintf(from, sizeof(from), "%s", call->from);
    mux_call_put(call);
    if (v != RADIO_MUX_MOVE) {
        return v;
    }
    // The mover is called on the caller's task, never the owner's: its probe goes through the mux. It answers once the
    // probe is done (a stop has none: once accepted); the rest runs on br_core's task (a long operation).
    char why[RADIO_MUX_REASON_MAX] = "";
    const char *to = stop ? "" : id;
    const esp_err_t err = mover(from, to, why, sizeof(why));

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "moving otbr from %s to %s: %s (%s)", from[0] ? from : "(none)", to[0] ? to : "(none)",
                 esp_err_to_name(err), why);
        if (reason && reason_cap > 0) {
            snprintf(reason, reason_cap, "%s", why);
        }
        return err == ESP_ERR_INVALID_RESPONSE ? RADIO_MUX_REFUSE_PROBE_FAILED :
               err == ESP_ERR_INVALID_STATE ? RADIO_MUX_REFUSE_BUSY : RADIO_MUX_REFUSE_FAILED;
    }
    if (moved_from && cap > 0) {
        snprintf(moved_from, cap, "%s", from);
    }
    return RADIO_MUX_MOVE; // the move stores the modes; it carries no line or rcp_proxy settings (handle_set)
}

bool radio_mux_started(void)
{
    return atomic_load(&g_mux.started);
}

esp_err_t radio_mux_forget(const char *id)
{
    ESP_RETURN_ON_FALSE(id && atomic_load(&g_mux.started), ESP_ERR_INVALID_STATE, TAG, "not started");
    mux_call_t *call = call_new(id);

    ESP_RETURN_ON_FALSE(call, ESP_ERR_NO_MEM, TAG, "call");
    esp_err_t err = call_owner(MUX_MSG_FORGET, call);

    if (err == ESP_OK) {
        err = call->err;
    }
    mux_call_put(call);
    return err;
}

void mux_maint_finish(const char *id)
{
    const radio_mux_builtin_t *b = g_mux.builtin;
    mux_call_t *call = call_new_end(id);

    call->steps = b ? b->boot_steps(&call->kind, call->reason, sizeof(call->reason)) : ESP_OK;
    call_owner_end(MUX_MSG_MAINT_DONE, call, "reinit end");
    mux_call_put(call);
}

esp_err_t radio_mux_maintain(const char *id, radio_mux_work_t work, const void *source, char *detail, size_t cap,
                             radio_mux_maint_t *out)
{
    const radio_mux_builtin_t *b = g_mux.builtin;
    radio_mux_maint_t res = {0};

    if (detail && cap > 0) {
        detail[0] = '\0';
    }
    if (out) {
        *out = res;
    }
    ESP_RETURN_ON_FALSE(id && atomic_load(&g_mux.started), ESP_ERR_INVALID_STATE, TAG, "not started");
    ESP_RETURN_ON_FALSE(b, ESP_ERR_NOT_SUPPORTED, TAG, "no built-in RCP");
    ESP_RETURN_ON_FALSE(work == RADIO_MUX_WORK_PUSH || work == RADIO_MUX_WORK_ROM_RECOVERY, ESP_ERR_INVALID_ARG, TAG,
                        "not a maintenance: %d", (int)work);
    mux_call_t *call = call_new(id);

    ESP_RETURN_ON_FALSE(call, ESP_ERR_NO_MEM, TAG, "call");
    call->work = work;
    // The owner answers by the instance's deadline (RADIO_MUX_REINIT_REPLY_MS) at the latest.
    esp_err_t err = call_owner_ms(MUX_MSG_MAINT_ASK, call, RADIO_MUX_REINIT_REPLY_MS + 2 * CALL_WAIT_MS);

    if (err == ESP_ERR_TIMEOUT) {
        snprintf(detail, cap, "the radio mux did not answer");
        mux_call_put(call);
        return ESP_ERR_TIMEOUT; // not posted: nothing asked
    }
    if (err == ESP_ERR_NOT_FINISHED) {
        int pending = MUX_CALL_PENDING;

        if (atomic_compare_exchange_strong(&call->claim, &pending, MUX_CALL_ABANDONED)) {
            snprintf(detail, cap, "the radio mux did not answer");
            mux_call_put(call);
            return ESP_ERR_TIMEOUT; // the owner ends the reinit by itself if it ever runs: nothing ran here
        }
        xSemaphoreTake(call->done, portMAX_DELAY); // taken just now: the channel is in reinit
    }
    err = call->err;
    if (err != ESP_OK && detail && cap > 0) {
        snprintf(detail, cap, "%s", call->reason);
    }
    mux_call_put(call);
    if (err != ESP_OK) {
        return err;
    }
    // The channel is in reinit: the work, then the boot steps; the owner then makes it ready or a fault again.
    res.ran = true;
    err = work == RADIO_MUX_WORK_PUSH ? b->push(source, detail, cap, &res.base_only) : b->rom_recover(detail, cap);
    mux_maint_finish(id);
    if (out) {
        *out = res;
    }
    return err;
}

bool radio_mux_maintenance_running(void)
{
    return atomic_load(&g_mux.maint_running);
}

bool radio_mux_otbr_channel(char *out, size_t cap)
{
    bool found = false;

    if (cap > 0) {
        out[0] = '\0';
    }
    if (!atomic_load(&g_mux.started)) {
        return false;
    }
    mux_lock();
    for (size_t i = 0; i < RADIO_MUX_CHANNELS_MAX && !found; i++) {
        if (g_mux.chans[i].used && g_mux.chans[i].mode == RADIO_MUX_MODE_OTBR) {
            snprintf(out, cap, "%s", g_mux.chans[i].id); // the record may change once the lock is released
            found = true;
        }
    }
    mux_unlock();
    return found;
}

void radio_mux_set_otbr_mover(esp_err_t (*move)(const char *from, const char *to, char *reason, size_t cap))
{
    atomic_store(&g_mux.mover, move);
}

// A call the owner may skip when its caller gave up (LEASE, COMMIT_OTBR): ESP_ERR_TIMEOUT then means nothing ran.
static esp_err_t call_owner_claimed(mux_msg_type_t type, mux_call_t *call)
{
    esp_err_t err = call_owner(type, call);

    if (err == ESP_ERR_TIMEOUT) {
        return ESP_ERR_TIMEOUT; // not posted: nothing ran
    }
    if (err == ESP_ERR_NOT_FINISHED) {
        int pending = MUX_CALL_PENDING;

        if (atomic_compare_exchange_strong(&call->claim, &pending, MUX_CALL_ABANDONED)) {
            return ESP_ERR_TIMEOUT;
        }
        xSemaphoreTake(call->done, portMAX_DELAY); // the owner took it just now: it runs to the end
        err = ESP_OK;
    }
    return err == ESP_OK ? call->err : err;
}

esp_err_t radio_mux_lease(const char *id, radio_mux_chan_t **out, char *err, size_t cap)
{
    if (err && cap > 0) {
        err[0] = '\0';
    }
    ESP_RETURN_ON_FALSE(id && out && atomic_load(&g_mux.started), ESP_ERR_INVALID_STATE, TAG, "not started");
    *out = NULL;
    mux_call_t *call = call_new(id);

    ESP_RETURN_ON_FALSE(call, ESP_ERR_NO_MEM, TAG, "call");
    const esp_err_t ret = call_owner_claimed(MUX_MSG_LEASE, call);

    if (ret == ESP_OK) {
        *out = call->handle;
    } else if (err && cap > 0) {
        snprintf(err, cap, "%s", ret == ESP_ERR_TIMEOUT ? "the radio mux did not answer" : call->reason);
    }
    mux_call_put(call);
    return ret;
}

void radio_mux_lease_end(radio_mux_chan_t *ch, const char *fault)
{
    const char *id = atomic_load(&g_mux.started) ? radio_mux_chan_id(ch) : NULL;

    if (!id) {
        return;
    }
    mux_call_t *call = call_new_end(id);

    call->handle = ch;
    snprintf(call->reason, sizeof(call->reason), "%s", fault ? fault : "");
    call_owner_end(MUX_MSG_LEASE_END, call, "lease end");
    mux_call_put(call);
}

esp_err_t radio_mux_commit_otbr(const char *from, const char *to)
{
    ESP_RETURN_ON_FALSE(atomic_load(&g_mux.started), ESP_ERR_INVALID_STATE, TAG, "not started");
    ESP_RETURN_ON_FALSE((to && to[0]) || (from && from[0]), ESP_ERR_INVALID_ARG, TAG, "neither a target nor a source");
    mux_call_t *call = call_new(to ? to : "");

    ESP_RETURN_ON_FALSE(call, ESP_ERR_NO_MEM, TAG, "call");
    snprintf(call->from, sizeof(call->from), "%s", from ? from : "");
    const esp_err_t ret = call_owner_claimed(MUX_MSG_COMMIT_OTBR, call);

    mux_call_put(call);
    return ret;
}

void mux_call_put(mux_call_t *call)
{
    if (atomic_fetch_sub(&call->refs, 1) == 1) {
        vSemaphoreDelete(call->done);
        free(call);
    }
}
