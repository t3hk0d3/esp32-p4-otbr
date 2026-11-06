// Delivery of a transport's received data (spec section 3, Frames, events and copies): the raw and detection ring,
// the frame-pool slots, the HDLC decode into slots, the events to the consumer and the eventfd. Runs in the
// transports' callbacks (rx_lock held) and on the owner task; consumers reach it through release, poll and read_raw.
#include <stdatomic.h>
#include <string.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "radio_mux_priv.h"

static const char *TAG = "radio_mux";

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

size_t mux_ring_take(mux_port_t *p, uint8_t *out, size_t cap)
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

void mux_ring_flush(mux_port_t *p)
{
    size_t got;
    void *d;

    while (p->ring && (d = xRingbufferReceiveUpTo(p->ring, &got, 0, MUX_RX_RING_SIZE)) != NULL) {
        vRingbufferReturnItem(p->ring, d);
    }
}

// Clears the eventfd, then sets it again when something is still waiting: an event or bytes that arrived between the
// last take and the clear are never left without a wakeup.
void mux_rearm(mux_port_t *p)
{
    uint64_t v;

    if (p->efd < 0 || read(p->efd, &v, sizeof(v)) != sizeof(v)) {
        return;
    }
    if (uxQueueMessagesWaiting(p->evq) > 0 || ring_waiting(p) > 0) {
        poke(p);
    }
}

void mux_slot_put(mux_port_t *p, uint8_t *frame)
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
        mux_slot_put(p, frame);
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
void mux_set_rx(mux_port_t *p, mux_rx_mode_t mode)
{
    xSemaphoreTake(p->rx_lock, portMAX_DELAY);
    if (p->dec_slot) {
        mux_slot_put(p, p->dec_slot + MUX_SLOT_HDR);
        p->dec_slot = NULL;
    }
    mux_ring_flush(p);
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
void mux_post_ctl(mux_port_t *p, radio_mux_ev_type_t type)
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
void mux_drain_evq(mux_port_t *p)
{
    radio_mux_ev_t ev;

    while (xQueueReceive(p->evq, &ev, 0) == pdTRUE) {
        if (ev.type == RADIO_MUX_EV_RX_FRAME) {
            mux_slot_put(p, ev.slot);
        }
    }
    mux_rearm(p);
}
