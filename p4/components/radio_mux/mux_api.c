// The consumer side (bind, poll, release, tx, reset, raw reads, firmware notes) and the API side (snapshot, set,
// forget, otbr channel, mover) of the mux. Calls that change a channel post a message and wait for the owner task.
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "radio_mux_priv.h"

static const char *TAG = "radio_mux";

#define CALL_WAIT_MS 5000

/* ---- registration ---- */

esp_err_t radio_mux_register_consumer(const radio_mux_consumer_t *c)
{
    ESP_RETURN_ON_FALSE(c && c->attach && c->detach, ESP_ERR_INVALID_ARG, TAG, "consumer needs attach and detach");
    ESP_RETURN_ON_FALSE(c->mode == RADIO_MUX_MODE_OTBR || c->mode == RADIO_MUX_MODE_ZIGGURAT ||
                        c->mode == RADIO_MUX_MODE_RCP_PROXY, ESP_ERR_INVALID_ARG, TAG, "no consumer for mode %d",
                        (int)c->mode);
    ESP_RETURN_ON_FALSE(!atomic_load(&g_mux.started), ESP_ERR_INVALID_STATE, TAG, "register before radio_mux_start");
    ESP_RETURN_ON_FALSE(!mux_find_consumer(c->mode), ESP_ERR_INVALID_STATE, TAG, "%s already has a consumer",
                        radio_mux_mode_name(c->mode));
    ESP_RETURN_ON_FALSE(g_mux.n_consumers < RADIO_MUX_RULES_MAX, ESP_ERR_NO_MEM, TAG, "too many consumers");
    g_mux.consumers[g_mux.n_consumers++] = *c;
    g_mux.rules.rules[g_mux.rules.n++] = (radio_mux_rule_t) {
        .mode = c->mode, .needs = c->needs, .limit = c->limit, .reboot_on_change = c->reboot_on_change,
    };
    ESP_LOGI(TAG, "%s consumer registered", radio_mux_mode_name(c->mode));
    return ESP_OK;
}

/* ---- consumer side ---- */

esp_err_t radio_mux_bind_queue(radio_mux_chan_t *ch, QueueHandle_t q)
{
    ESP_RETURN_ON_FALSE(ch && atomic_load(&g_mux.started), ESP_ERR_INVALID_STATE, TAG, "not started");
    mux_port_t *p = ch->port;
    radio_mux_ev_t ev;

    ESP_RETURN_ON_FALSE(atomic_load(&p->evq_chan) == ch, ESP_ERR_INVALID_STATE, TAG, "%s is not attached", ch->id);
    xSemaphoreTake(p->rx_lock, portMAX_DELAY);
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
    xSemaphoreGive(p->rx_lock);
    return ESP_OK;
}

int radio_mux_eventfd(radio_mux_chan_t *ch)
{
    return ch && atomic_load(&g_mux.started) ? ch->port->efd : -1;
}

bool radio_mux_poll(radio_mux_chan_t *ch, radio_mux_ev_t *ev)
{
    if (!ch || !atomic_load(&g_mux.started)) {
        return false;
    }
    mux_port_t *p = ch->port;

    if (atomic_load(&p->evq_chan) != ch) {
        return false; // a stale handle: the queue belongs to the consumer of another channel now
    }
    if (xQueueReceive(p->evq, ev, 0) == pdTRUE) {
        return true;
    }
    mux_rearm(p);
    return false;
}

void radio_mux_release(radio_mux_chan_t *ch, uint8_t *slot)
{
    if (ch && slot) {
        mux_slot_put(ch->port, slot);
    }
}

esp_err_t radio_mux_tx(radio_mux_chan_t *ch, const uint8_t *data, size_t n)
{
    if (!ch || !atomic_load(&g_mux.started)) {
        return ESP_ERR_INVALID_STATE;
    }
    mux_port_t *p = ch->port;

    if (!atomic_load(&p->tx_ok) || atomic_load(&p->evq_chan) != ch) {
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
    if (!atomic_load(&p->tx_ok) || atomic_load(&p->evq_chan) != ch) {
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
    ESP_RETURN_ON_FALSE(ch && atomic_load(&g_mux.started), ESP_ERR_INVALID_STATE, TAG, "not started");
    const mux_msg_t m = {.type = MUX_MSG_RESET, .ch = ch};

    return mux_post(&m) ? ESP_OK : ESP_ERR_NO_MEM;
}

size_t radio_mux_read_raw(radio_mux_chan_t *ch, uint8_t *out, size_t cap)
{
    if (!ch || !atomic_load(&g_mux.started) || !ch->port->ring) {
        return 0;
    }
    mux_port_t *p = ch->port;

    if (atomic_load(&p->evq_chan) != ch) {
        return 0; // a stale handle: the ring belongs to the consumer of another channel now
    }
    const size_t n = mux_ring_take(p, out, cap);

    if (n == 0) {
        mux_rearm(p);
    }
    return n;
}

bool radio_mux_chan_frames(const radio_mux_chan_t *ch)
{
    if (!ch) {
        return false;
    }
    if (ch->kind == RADIO_MUX_KIND_C6) {
        return true;
    }
    const mux_port_t *p = ch->port;

    return atomic_load(&p->evq_chan) == ch && p->tx_frames;
}

const char *radio_mux_chan_id(const radio_mux_chan_t *ch)
{
    return ch ? ch->id : NULL;
}

void radio_mux_get_proxy(const radio_mux_chan_t *ch, radio_mux_proxy_settings_t *out)
{
    if (!ch || !g_mux.lock) {
        radio_mux_proxy_defaults(out);
        return;
    }
    mux_lock();
    *out = ch->proxy;
    mux_unlock();
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
}

void radio_mux_get_snapshot(radio_mux_snapshot_t *out)
{
    out->n = 0;
    if (!atomic_load(&g_mux.started)) {
        return;
    }
    mux_lock();
    for (size_t i = 0; i < RADIO_MUX_CHANNELS_MAX; i++) {
        if (g_mux.chans[i].used) {
            fill_info(&g_mux.chans[i], &out->chans[out->n++]);
        }
    }
    mux_unlock();
}

// Posts a call to the owner and waits for it. ESP_ERR_TIMEOUT when the owner did not answer in time: the record is
// then freed by the owner when it gets to it.
static esp_err_t call_owner(mux_msg_type_t type, mux_call_t *call)
{
    const mux_msg_t m = {.type = type, .call = call};

    if (xQueueSend(g_mux.queue, &m, pdMS_TO_TICKS(CALL_WAIT_MS)) != pdTRUE) {
        mux_call_put(call); // the owner's reference
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
    if (!id || !req || !atomic_load(&g_mux.started)) {
        return RADIO_MUX_REFUSE_FAILED;
    }
    esp_err_t (*mover)(const char *, const char *) = atomic_load(&g_mux.mover);
    mux_call_t *call = call_new(id);

    if (!call) {
        return RADIO_MUX_REFUSE_FAILED;
    }
    call->req = *req;
    call->req.kick = false; // never stored; device_api kicks through rcp_proxy (R6)
    call->have_mover = mover != NULL;
    if (call_owner(MUX_MSG_SET, call) != ESP_OK) {
        mux_call_put(call);
        return RADIO_MUX_REFUSE_FAILED;
    }
    radio_mux_verdict_t v = call->verdict;
    char from[RADIO_MUX_ID_MAX];

    snprintf(from, sizeof(from), "%s", call->from);
    mux_call_put(call);
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

const char *radio_mux_otbr_channel(void)
{
    const char *id = NULL;

    if (!atomic_load(&g_mux.started)) {
        return NULL;
    }
    mux_lock();
    for (size_t i = 0; i < RADIO_MUX_CHANNELS_MAX && !id; i++) {
        if (g_mux.chans[i].used && g_mux.chans[i].mode == RADIO_MUX_MODE_OTBR) {
            id = g_mux.chans[i].id;
        }
    }
    mux_unlock();
    return id;
}

void radio_mux_set_otbr_mover(esp_err_t (*move)(const char *from, const char *to))
{
    atomic_store(&g_mux.mover, move);
}

void mux_call_put(mux_call_t *call)
{
    if (atomic_fetch_sub(&call->refs, 1) == 1) {
        vSemaphoreDelete(call->done);
        free(call);
    }
}
