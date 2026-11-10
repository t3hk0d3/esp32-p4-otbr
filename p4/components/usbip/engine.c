#include "usbip_engine.h"

#include <inttypes.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "usbip";

#define SETUP_LEN 8
#define EP0_MPS_MAX 64  // control IN data stages are rounded up to this (at least the device's bMaxPacketSize0)
#define DESC_INTERFACE 4
#define TYPE_CONTROL 0
#define TYPE_ISO 1
#define SLOTS (USBIP_EP_MAX + 1)

// What a flushed transfer becomes when it comes back canceled.
typedef enum {
    FATE_RESUBMIT,  // collateral of an unlink or a toggle reset: submitted again, in order
    FATE_RESET,     // its interface changed or its endpoint's halt was cleared: answered -ECONNRESET
} fate_t;

struct usbip_engine_xfer {
    usbip_xfer_t x;  // first, so a completion maps back with a cast
    usbip_engine_xfer_t *next;
    uint8_t *wait_data;  // OUT data while waiting (PSRAM)
    size_t length;       // the CMD_SUBMIT's transfer_buffer_length
    size_t grant;        // budget bytes held while in flight
    size_t wait_bytes;   // waiting bytes accounted while in the FIFO
    int64_t deadline;    // EP0 unlink deadline
    uint32_t unlink_seq; // the CMD_UNLINK that targets this transfer
    uint8_t slot;
    uint8_t setup[SETUP_LEN];
    uint8_t req;         // usbip_req_t of an EP0 transfer
    uint8_t req_iface, req_alt, req_prev_alt;
    uint8_t fate;
    bool unlinked;       // a CMD_UNLINK waits for this transfer's completion
    bool flushed;        // in a flush; counted in the slot's canceling
    bool returned;       // came back canceled from a flush, waits to be submitted again
    bool orphan;         // its session ended while the USB side had it
    bool sent;           // its OUT bytes are counted
};

// --- Lists, replies, memory ---

static void list_push(usbip_engine_xfer_t **head, usbip_engine_xfer_t **tail, usbip_engine_xfer_t *t)
{
    t->next = NULL;
    if (*tail != NULL) {
        (*tail)->next = t;
    } else {
        *head = t;
    }
    *tail = t;
}

static void list_remove(usbip_engine_xfer_t **head, usbip_engine_xfer_t **tail, usbip_engine_xfer_t *t)
{
    usbip_engine_xfer_t *prev = NULL;

    for (usbip_engine_xfer_t *p = *head; p != NULL; prev = p, p = p->next) {
        if (p != t) {
            continue;
        }
        if (prev != NULL) {
            prev->next = p->next;
        } else {
            *head = p->next;
        }
        if (*tail == p) {
            *tail = prev;
        }
        t->next = NULL;
        return;
    }
}

static void ops_free(usbip_engine_t *e, void *p)
{
    if (p != NULL) {
        e->ops->free(e->ctx, p);
    }
}

static void xfer_free(usbip_engine_t *e, usbip_engine_xfer_t *t)
{
    ops_free(e, t->x.buf);
    ops_free(e, t->wait_data);
    ops_free(e, t);
}

static void reply_submit(usbip_engine_t *e, uint32_t seq, int32_t status, size_t actual, const uint8_t *data, size_t n)
{
    usbip_ret_submit_t r = {.seqnum = seq, .status = status, .actual_length = (int32_t)actual};
    uint8_t hdr[USBIP_ENGINE_HDR_LEN];

    usbip_proto_encode_ret_submit(&r, hdr);
    e->ops->emit(e->ctx, hdr, data, n);
    if (status == 0) {
        e->counters.completed++;
    } else {
        e->counters.errors++;
    }
    e->counters.bytes_in += n;
}

static void reply_unlink(usbip_engine_t *e, uint32_t seq, int32_t status)
{
    uint8_t hdr[USBIP_ENGINE_HDR_LEN];

    usbip_proto_encode_ret_unlink(seq, status, hdr);
    e->ops->emit(e->ctx, hdr, NULL, 0);
}

static void end(usbip_engine_t *e, const char *why)
{
    ESP_LOGW(TAG, "ending the session: %s", why);
    e->ops->end_session(e->ctx, why);
    usbip_engine_stop(e, why);
}

// --- Slots and interfaces ---

static usbip_engine_slot_t *slot_by_addr(usbip_engine_t *e, uint8_t addr)
{
    for (int i = 1; i < SLOTS; i++) {
        if (e->slots[i].claimed && e->slots[i].addr == addr) {
            return &e->slots[i];
        }
    }
    return NULL;
}

static usbip_engine_iface_t *iface_find(usbip_engine_t *e, uint8_t num)
{
    for (int i = 0; i < e->num_ifaces; i++) {
        if (e->ifaces[i].num == num) {
            return &e->ifaces[i];
        }
    }
    return NULL;
}

// Gives each non-iso endpoint of iface at alt a slot: the slot that still holds this address's transfers, else a free
// one. Iso endpoints get none, so a SUBMIT to them answers -EINVAL.
static void slots_claim(usbip_engine_t *e, uint8_t iface, uint8_t alt)
{
    usbip_ep_info_t eps[USBIP_EP_MAX];
    size_t n = usbip_core_endpoints(e->cfg, e->cfg_len, iface, alt, eps, USBIP_EP_MAX);

    for (size_t i = 0; i < n && i < USBIP_EP_MAX; i++) {
        if (eps[i].type == TYPE_ISO) {
            continue;
        }
        usbip_engine_slot_t *s = NULL;

        for (int k = 1; k < SLOTS && s == NULL; k++) {
            usbip_engine_slot_t *c = &e->slots[k];

            if (!c->claimed && c->addr == eps[i].addr && (c->inflight != NULL || c->wait != NULL)) {
                s = c;
            }
        }
        for (int k = 1; k < SLOTS && s == NULL; k++) {
            usbip_engine_slot_t *c = &e->slots[k];

            if (!c->claimed && c->inflight == NULL && c->wait == NULL) {
                s = c;
            }
        }
        if (s == NULL) {
            ESP_LOGW(TAG, "no endpoint slot for 0x%02x (interface %u alt %u)", eps[i].addr, iface, alt);
            continue;
        }
        s->claimed = true;
        s->addr = eps[i].addr;
        s->type = eps[i].type;
        s->mps = eps[i].mps;
        s->iface = iface;
    }
}

// The interface's endpoints stop taking transfers; their waiting transfers are answered -ECONNRESET.
static void slots_unclaim(usbip_engine_t *e, uint8_t iface)
{
    for (int i = 1; i < SLOTS; i++) {
        usbip_engine_slot_t *s = &e->slots[i];

        if (!s->claimed || s->iface != iface) {
            continue;
        }
        s->claimed = false;
        while (s->wait != NULL) {
            usbip_engine_xfer_t *t = s->wait;

            list_remove(&s->wait, &s->wait_tail, t);
            usbip_budget_wait_sub(&e->budget, t->wait_bytes);
            reply_submit(e, t->x.seqnum, -USBIP_ECONNRESET, 0, NULL, 0);
            xfer_free(e, t);
        }
    }
}

static bool iface_claim(usbip_engine_t *e, usbip_engine_iface_t *f, uint8_t alt)
{
    esp_err_t err = e->ops->iface_claim(e->ctx, f->num, alt);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "claiming interface %u alt %u: %s", f->num, alt, esp_err_to_name(err));
        return false;
    }
    f->alt = alt;
    f->claimed = true;
    slots_claim(e, f->num, alt);
    return true;
}

static void iface_release(usbip_engine_t *e, usbip_engine_iface_t *f)
{
    slots_unclaim(e, f->num);
    f->claimed = false;
    esp_err_t err = e->ops->iface_release(e->ctx, f->num);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "releasing interface %u: %s", f->num, esp_err_to_name(err));
    }
}

// --- Transfers ---

static size_t round_up(size_t n, size_t m)
{
    return m == 0 ? n : (n + m - 1) / m * m;
}

// The bytes an in-flight transfer moves (esp-usb num_bytes), which is also what the budget counts.
static size_t xfer_size(const usbip_engine_t *e, const usbip_engine_xfer_t *t)
{
    if (t->slot == 0) {
        return SETUP_LEN + (t->x.in ? round_up(t->length, EP0_MPS_MAX) : t->length);
    }
    return t->x.in ? round_up(t->length, e->slots[t->slot].mps) : t->length;
}

static bool grant(usbip_engine_t *e, usbip_engine_xfer_t *t)
{
    if (e->slots[t->slot].canceling > 0) {
        return false;
    }
    size_t n = xfer_size(e, t);

    if (!usbip_budget_grant(&e->budget, t->slot, t->x.in, n)) {
        return false;
    }
    t->grant = n;
    return true;
}

// Takes an in-flight transfer out of the engine: frees its slot share and its memory.
static void retire(usbip_engine_t *e, usbip_engine_xfer_t *t)
{
    usbip_engine_slot_t *s = &e->slots[t->slot];

    list_remove(&s->inflight, &s->inflight_tail, t);
    usbip_budget_release(&e->budget, t->slot, t->grant);
    xfer_free(e, t);
}

static void restore_alt(usbip_engine_t *e, usbip_engine_xfer_t *t)
{
    usbip_engine_iface_t *f = iface_find(e, t->req_iface);

    if (f != NULL && !f->claimed) {
        iface_claim(e, f, t->req_prev_alt);
    }
}

// Hands the transfer to the USB side. On a refusal it is answered -EPROTO and retired; false then.
static bool submit_now(usbip_engine_t *e, usbip_engine_xfer_t *t)
{
    esp_err_t err = e->ops->submit(e->ctx, &t->x);

    if (err == ESP_OK) {
        if (!t->sent && !t->x.in) {
            e->counters.bytes_out += t->length;
        }
        t->sent = true;
        return true;
    }
    ESP_LOGW(TAG, "submit of seqnum %" PRIu32 " on 0x%02x: %s", t->x.seqnum, t->x.ep, esp_err_to_name(err));
    if (t->req == USBIP_REQ_SET_INTERFACE) {
        restore_alt(e, t);
    }
    reply_submit(e, t->x.seqnum, -USBIP_EPROTO, 0, NULL, 0);
    if (t->unlinked) {
        reply_unlink(e, t->unlink_seq, 0);
    }
    retire(e, t);
    return false;
}

// Cancels what is in flight on s (halt, flush, clear); each transfer comes back through on_complete. fate says what a
// canceled one becomes; RESET also applies to transfers that are back already and wait to be submitted again.
static void flush_slot(usbip_engine_t *e, usbip_engine_slot_t *s, fate_t fate)
{
    bool any = false;

    for (usbip_engine_xfer_t *t = s->inflight, *next; t != NULL; t = next) {
        next = t->next;
        if (t->returned) {
            if (fate == FATE_RESET) {
                reply_submit(e, t->x.seqnum, -USBIP_ECONNRESET, 0, NULL, 0);
                retire(e, t);
            }
        } else if (t->flushed) {
            if (fate == FATE_RESET) {
                t->fate = FATE_RESET;
            }
        } else {
            t->flushed = true;
            t->fate = fate;
            s->canceling++;
            any = true;
        }
    }
    if (any) {
        e->ops->ep_halt(e->ctx, s->addr);
        e->ops->ep_flush(e->ctx, s->addr);
        e->ops->ep_clear(e->ctx, s->addr);
    }
}

// SET_INTERFACE, before it is forwarded: cancel the interface's transfers and release it (spec 6.4).
static void set_interface_begin(usbip_engine_t *e, usbip_engine_xfer_t *t)
{
    usbip_engine_iface_t *f = iface_find(e, t->setup[4]);

    if (f == NULL || !f->claimed) {
        t->req = USBIP_REQ_FORWARD;
        return;
    }
    t->req_iface = f->num;
    t->req_alt = t->setup[2];
    t->req_prev_alt = f->alt;
    for (int i = 1; i < SLOTS; i++) {
        if (e->slots[i].claimed && e->slots[i].iface == f->num) {
            flush_slot(e, &e->slots[i], FATE_RESET);
        }
    }
    iface_release(e, f);
}

// Starts a granted transfer: DMA buffer, copy, submit. out is the OUT data (from the command or the PSRAM FIFO).
static void start_xfer(usbip_engine_t *e, usbip_engine_xfer_t *t, const uint8_t *out)
{
    usbip_engine_slot_t *s = &e->slots[t->slot];
    uint8_t *buf = e->ops->alloc(e->ctx, t->grant > 0 ? t->grant : 1, true);

    if (buf == NULL) {
        usbip_budget_release(&e->budget, t->slot, t->grant);
        reply_submit(e, t->x.seqnum, -USBIP_ENOMEM, 0, NULL, 0);
        xfer_free(e, t);
        return;
    }
    size_t off = 0;

    if (t->slot == 0) {
        memcpy(buf, t->setup, SETUP_LEN);
        off = SETUP_LEN;
    }
    if (!t->x.in && t->length > 0 && out != NULL) {
        memcpy(buf + off, out, t->length);
    }
    t->x.buf = buf;
    t->x.len = t->grant;
    list_push(&s->inflight, &s->inflight_tail, t);
    if (t->req == USBIP_REQ_SET_INTERFACE) {
        set_interface_begin(e, t);
    }
    submit_now(e, t);
}

// Queues t in its endpoint's FIFO. Waiting data over USBIP_WAIT_BYTES ends the session (spec 8.1).
static void wait_push(usbip_engine_t *e, usbip_engine_xfer_t *t, const uint8_t *out)
{
    usbip_engine_slot_t *s = &e->slots[t->slot];
    size_t data = t->x.in ? 0 : t->length;
    size_t bytes = sizeof(*t) + data;

    if (!usbip_budget_wait_add(&e->budget, bytes)) {
        xfer_free(e, t);
        e->counters.overload_ends++;
        end(e, "waiting transfers exceed the PSRAM cap");
        return;
    }
    if (data > 0) {
        t->wait_data = e->ops->alloc(e->ctx, data, false);
        if (t->wait_data == NULL) {
            usbip_budget_wait_sub(&e->budget, bytes);
            reply_submit(e, t->x.seqnum, -USBIP_ENOMEM, 0, NULL, 0);
            xfer_free(e, t);
            return;
        }
        memcpy(t->wait_data, out, data);
    }
    t->wait_bytes = bytes;
    list_push(&s->wait, &s->wait_tail, t);
}

// Starts the head of slot i's FIFO when it gets a slot. True when one went out.
static bool pump_one(usbip_engine_t *e, int i)
{
    usbip_engine_slot_t *s = &e->slots[i];
    usbip_engine_xfer_t *t = s->wait;

    if (t == NULL || !grant(e, t)) {
        return false;
    }
    list_remove(&s->wait, &s->wait_tail, t);
    usbip_budget_wait_sub(&e->budget, t->wait_bytes);
    t->wait_bytes = 0;
    uint8_t *data = t->wait_data;

    t->wait_data = NULL;
    start_xfer(e, t, data);
    ops_free(e, data);
    return true;
}

// Spec 8.1: the freed endpoint's FIFO first, then the other FIFOs round-robin, one transfer per endpoint per pass.
static void drain(usbip_engine_t *e, int first)
{
    while (e->active && pump_one(e, first)) {
    }
    bool progress = true;

    while (e->active && progress) {
        progress = false;
        for (int k = 0; k < SLOTS && e->active; k++) {
            progress |= pump_one(e, (e->rr + k) % SLOTS);
        }
    }
    e->rr = (uint8_t)((e->rr + 1) % SLOTS);
}

// --- Commands ---

static void on_submit(usbip_engine_t *e, const usbip_cmd_t *c, const uint8_t *out)
{
    e->counters.submits++;
    bool in = c->direction == USBIP_DIR_IN;
    uint8_t num = (uint8_t)(c->ep & 0x0f);
    usbip_ep_info_t info = {.addr = 0, .type = TYPE_CONTROL, .mps = EP0_MPS_MAX};
    const usbip_ep_info_t *ep = &info;
    int slot = 0;

    if (c->ep > 0x0f) {
        ep = NULL;
    } else if (num != 0) {
        usbip_engine_slot_t *s = slot_by_addr(e, (uint8_t)(num | (in ? 0x80 : 0)));

        if (s != NULL) {
            info = (usbip_ep_info_t) {
                .addr = s->addr, .type = s->type, .mps = s->mps
            };
            slot = (int)(s - e->slots);
        } else {
            ep = NULL;
        }
    }
    int32_t err = usbip_core_check_submit(c, e->devid, ep);

    if (err != 0) {
        reply_submit(e, c->seqnum, err, 0, NULL, 0);
        return;
    }
    usbip_req_t req = USBIP_REQ_FORWARD;

    if (slot == 0) {
        req = usbip_core_classify_setup(c->submit.setup, e->config_value);
        switch (req) {
        case USBIP_REQ_SET_CONFIG_SAME:
        case USBIP_REQ_PORT_RESET:
            reply_submit(e, c->seqnum, 0, 0, NULL, 0);
            return;
        case USBIP_REQ_SET_CONFIG_OTHER:
        case USBIP_REQ_SET_ADDRESS:
            reply_submit(e, c->seqnum, -USBIP_EPIPE, 0, NULL, 0);
            return;
        default:
            break;
        }
    }
    usbip_engine_xfer_t *t = e->ops->alloc(e->ctx, sizeof(*t), false);

    if (t == NULL) {
        reply_submit(e, c->seqnum, -USBIP_ENOMEM, 0, NULL, 0);
        return;
    }
    memset(t, 0, sizeof(*t));
    t->x = (usbip_xfer_t) {
        .seqnum = c->seqnum, .ep = info.addr, .in = in, .type = info.type, .flags = c->submit.flags,
    };
    t->length = (size_t)c->submit.length;
    t->slot = (uint8_t)slot;
    t->req = (uint8_t)req;
    memcpy(t->setup, c->submit.setup, SETUP_LEN);
    if (!in && t->length > 0 && out == NULL) {
        reply_submit(e, c->seqnum, -USBIP_EINVAL, 0, NULL, 0);
        xfer_free(e, t);
        return;
    }
    if (e->slots[slot].wait == NULL && grant(e, t)) {
        start_xfer(e, t, out);
    } else {
        wait_push(e, t, out);
    }
}

static usbip_engine_xfer_t *find_in(usbip_engine_xfer_t *head, uint32_t seq)
{
    for (usbip_engine_xfer_t *t = head; t != NULL; t = t->next) {
        if (t->x.seqnum == seq) {
            return t;
        }
    }
    return NULL;
}

// Spec 6.5. A transfer in flight is answered when it comes back: canceled -> RET_UNLINK -ECONNRESET alone; completed
// before the flush took it -> its RET_SUBMIT, then RET_UNLINK 0. Either way each seqnum gets one answer.
static void on_unlink(usbip_engine_t *e, const usbip_cmd_t *c)
{
    e->counters.unlinks++;
    uint32_t target = c->unlink.unlink_seqnum;

    for (int i = 0; i < SLOTS; i++) {
        usbip_engine_slot_t *s = &e->slots[i];
        usbip_engine_xfer_t *t = find_in(s->wait, target);

        if (t != NULL) {
            list_remove(&s->wait, &s->wait_tail, t);
            usbip_budget_wait_sub(&e->budget, t->wait_bytes);
            xfer_free(e, t);
            reply_unlink(e, c->seqnum, -USBIP_ECONNRESET);
            drain(e, i);
            return;
        }
        t = find_in(s->inflight, target);
        if (t == NULL) {
            continue;
        }
        if (t->unlinked) {
            reply_unlink(e, c->seqnum, 0);
        } else if (t->returned) {
            reply_unlink(e, c->seqnum, -USBIP_ECONNRESET);
            retire(e, t);
            drain(e, i);
        } else {
            t->unlinked = true;
            t->unlink_seq = c->seqnum;
            if (i == 0) {
                t->deadline = e->ops->now_ms(e->ctx) + USBIP_EP0_UNLINK_MS;
            } else if (!t->flushed) {
                flush_slot(e, s, FATE_RESUBMIT);
            }
        }
        return;
    }
    reply_unlink(e, c->seqnum, 0);
}

void usbip_engine_on_cmd(usbip_engine_t *e, const usbip_cmd_t *c, const uint8_t *out_data)
{
    if (e == NULL || c == NULL || !e->active) {
        return;
    }
    if (c->command == USBIP_CMD_SUBMIT) {
        on_submit(e, c, out_data);
    } else if (c->command == USBIP_CMD_UNLINK) {
        on_unlink(e, c);
    }
}

// --- Completions ---

// Sends t's RET_SUBMIT with an explicit status (and the RET_UNLINK 0 of an unlink that came too late).
static void answer_status(usbip_engine_t *e, usbip_engine_xfer_t *t, int32_t status, size_t got)
{
    size_t off = t->slot == 0 ? SETUP_LEN : 0;
    bool data = t->x.in && got > 0;

    reply_submit(e, t->x.seqnum, status, got, data ? t->x.buf + off : NULL, data ? got : 0);
    if (t->unlinked) {
        reply_unlink(e, t->unlink_seq, 0);
    }
}

static size_t data_got(const usbip_engine_xfer_t *t, size_t actual)
{
    size_t off = t->slot == 0 ? SETUP_LEN : 0;
    size_t got = actual > off ? actual - off : 0;

    return got > t->length ? t->length : got;
}

static void answer(usbip_engine_t *e, usbip_engine_xfer_t *t, usbip_xfer_status_t st, size_t actual)
{
    size_t got = data_got(t, actual);

    answer_status(e, t, usbip_core_status_errno(st, t->x.in && got < t->length, t->x.flags), got);
}

// CLEAR_FEATURE(ENDPOINT_HALT) went through: reset the host side's toggles of the endpoint's interface the only way
// esp-usb allows (spec 6.4): cancel its endpoints, release it and claim it again at its current alt. Transfers on
// the cleared endpoint get -ECONNRESET; the others are submitted again in order when they are back.
static void clear_halt_done(usbip_engine_t *e, uint8_t addr)
{
    usbip_engine_slot_t *target = slot_by_addr(e, addr);
    usbip_engine_iface_t *f = target != NULL ? iface_find(e, target->iface) : NULL;

    if (f == NULL || !f->claimed) {
        return;
    }
    for (int i = 1; i < SLOTS; i++) {
        usbip_engine_slot_t *s = &e->slots[i];

        if (s->claimed && s->iface == f->num) {
            flush_slot(e, s, s == target ? FATE_RESET : FATE_RESUBMIT);
        }
    }
    uint8_t alt = f->alt;

    // The FIFOs stay: release and claim again keep the same slots for the same endpoints.
    f->claimed = false;
    esp_err_t err = e->ops->iface_release(e->ctx, f->num);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "releasing interface %u: %s", f->num, esp_err_to_name(err));
    }
    err = e->ops->iface_claim(e->ctx, f->num, alt);
    if (err == ESP_OK) {
        f->claimed = true;
    } else {
        ESP_LOGW(TAG, "claiming interface %u alt %u: %s", f->num, alt, esp_err_to_name(err));
        slots_unclaim(e, f->num);
    }
}

static void complete_ep0_request(usbip_engine_t *e, usbip_engine_xfer_t *t, usbip_xfer_status_t st, size_t actual)
{
    if (t->req == USBIP_REQ_SET_INTERFACE) {
        usbip_engine_iface_t *f = iface_find(e, t->req_iface);
        int32_t status = usbip_core_status_errno(st, false, 0);

        if (status == 0 && (f == NULL || !iface_claim(e, f, t->req_alt))) {
            status = -USBIP_EPROTO;
        }
        if (status != 0) {
            restore_alt(e, t);
        }
        answer_status(e, t, status, 0);
        return;
    }
    if (st == USBIP_XS_COMPLETED) {
        clear_halt_done(e, t->setup[4]);
    }
    answer(e, t, st, actual);
}

// A transfer of a flush came back.
static void complete_flushed(usbip_engine_t *e, usbip_engine_slot_t *s, usbip_engine_xfer_t *t,
                             usbip_xfer_status_t st, size_t actual)
{
    t->flushed = false;
    s->canceling--;
    if (st != USBIP_XS_CANCELED) {
        // Done before the flush took it: an ordinary answer (the flush already cleared the endpoint).
        answer(e, t, st, actual);
        retire(e, t);
        if (st == USBIP_XS_NO_DEVICE) {
            end(e, "device gone");
            return;
        }
    } else if (t->unlinked) {
        reply_unlink(e, t->unlink_seq, -USBIP_ECONNRESET);
        retire(e, t);
    } else if (t->fate == FATE_RESET) {
        reply_submit(e, t->x.seqnum, -USBIP_ECONNRESET, 0, NULL, 0);
        retire(e, t);
    } else {
        t->returned = true;
    }
    if (s->canceling > 0) {
        return;
    }
    // The whole flush is back: submit the collateral transfers again in their original order, ahead of the FIFO.
    for (usbip_engine_xfer_t *r = s->inflight, *next; r != NULL && e->active; r = next) {
        next = r->next;
        if (!r->returned) {
            continue;
        }
        r->returned = false;
        if (s->claimed) {
            submit_now(e, r);
        } else {
            reply_submit(e, r->x.seqnum, -USBIP_ECONNRESET, 0, NULL, 0);
            retire(e, r);
        }
    }
}

static void complete_normal(usbip_engine_t *e, usbip_engine_slot_t *s, usbip_engine_xfer_t *t,
                            usbip_xfer_status_t st, size_t actual)
{
    if (t->slot == 0 && (t->req == USBIP_REQ_SET_INTERFACE || t->req == USBIP_REQ_CLEAR_HALT)) {
        complete_ep0_request(e, t, st, actual);
    } else {
        answer(e, t, st, actual);
    }
    bool halted = t->slot != 0 && (st == USBIP_XS_STALL || st == USBIP_XS_ERROR || st == USBIP_XS_OVERFLOW);

    retire(e, t);
    if (halted && s->claimed) {
        // After a STALL or an error the pipe halts; clearing it lets its queue continue (spec 6.3).
        e->ops->ep_clear(e->ctx, s->addr);
    }
    if (st == USBIP_XS_NO_DEVICE) {
        end(e, "device gone");
    }
}

void usbip_engine_on_complete(usbip_engine_t *e, usbip_xfer_t *x, usbip_xfer_status_t st, size_t actual)
{
    if (e == NULL || x == NULL) {
        return;
    }
    usbip_engine_xfer_t *t = (usbip_engine_xfer_t *)x;

    if (t->orphan || !e->active) {
        // Its session is over: the flush of session end canceled it. Nothing to answer.
        xfer_free(e, t);
        return;
    }
    int i = t->slot;
    usbip_engine_slot_t *s = &e->slots[i];

    if (t->flushed) {
        complete_flushed(e, s, t, st, actual);
    } else {
        complete_normal(e, s, t, st, actual);
    }
    if (e->active) {
        drain(e, i);
    }
}

void usbip_engine_tick(usbip_engine_t *e)
{
    if (e == NULL || !e->active) {
        return;
    }
    usbip_engine_xfer_t *t = e->slots[0].inflight;

    if (t != NULL && t->unlinked && e->ops->now_ms(e->ctx) >= t->deadline) {
        end(e, "control transfer stuck after an unlink");
    }
}

// --- Session ---

esp_err_t usbip_engine_start(usbip_engine_t *e, const usbip_engine_ops_t *ops, void *ctx, const usbip_dev_rec_t *dev,
                             const uint8_t *cfg, size_t cfg_len)
{
    ESP_RETURN_ON_FALSE(e != NULL && ops != NULL && dev != NULL && cfg != NULL && cfg_len >= 9, ESP_ERR_INVALID_ARG,
                        TAG, "engine start: bad arguments");
    ESP_RETURN_ON_FALSE(!e->active, ESP_ERR_INVALID_STATE, TAG, "engine start: a session runs");
    memset(e, 0, sizeof(*e));
    e->ops = ops;
    e->ctx = ctx;
    e->devid = (dev->busnum << 16) | dev->devnum;
    e->config_value = dev->config_value;
    e->cfg = cfg;
    e->cfg_len = cfg_len;
    usbip_budget_init(&e->budget);

    size_t total = (size_t)cfg[2] | ((size_t)cfg[3] << 8);

    if (total > cfg_len) {
        total = cfg_len;
    }
    for (size_t o = 0; o + 2 <= total && cfg[o] >= 2 && cfg[o] <= total - o; o += cfg[o]) {
        const uint8_t *d = &cfg[o];

        if (d[1] != DESC_INTERFACE || d[0] < 9 || d[3] != 0 || iface_find(e, d[2]) != NULL ||
                e->num_ifaces >= USBIP_IFACES_MAX) {
            continue;
        }
        e->ifaces[e->num_ifaces++] = (usbip_engine_iface_t) {
            .num = d[2]
        };
    }

    e->slots[0] = (usbip_engine_slot_t) {
        .claimed = true, .type = TYPE_CONTROL, .mps = EP0_MPS_MAX
    };
    for (int i = 0; i < e->num_ifaces; i++) {
        esp_err_t err = ops->iface_claim(ctx, e->ifaces[i].num, 0);

        if (err != ESP_OK) {
            ESP_LOGW(TAG, "claiming interface %u: %s", e->ifaces[i].num, esp_err_to_name(err));
            for (int k = 0; k < i; k++) {
                ops->iface_release(ctx, e->ifaces[k].num);
            }
            e->num_ifaces = 0;
            return err;
        }
        e->ifaces[i].claimed = true;
        slots_claim(e, e->ifaces[i].num, 0);
    }
    e->active = true;
    return ESP_OK;
}

void usbip_engine_stop(usbip_engine_t *e, const char *why)
{
    if (e == NULL || !e->active) {
        return;
    }
    e->active = false;
    ESP_LOGI(TAG, "session ended: %s", why != NULL ? why : "");

    for (int i = 1; i < SLOTS; i++) {
        if (e->slots[i].claimed) {
            e->ops->ep_halt(e->ctx, e->slots[i].addr);
            e->ops->ep_flush(e->ctx, e->slots[i].addr);
            e->ops->ep_clear(e->ctx, e->slots[i].addr);
        }
    }
    // EP0 cannot be flushed: a pending control transfer ends only when the root port is off (spec 5).
    bool ep0_pending = e->slots[0].inflight != NULL;

    if (ep0_pending) {
        e->ops->port_power_cycle(e->ctx);
    }
    for (int i = 0; i < e->num_ifaces; i++) {
        if (e->ifaces[i].claimed) {
            e->ifaces[i].claimed = false;
            e->ops->iface_release(e->ctx, e->ifaces[i].num);
        }
    }
    if (!ep0_pending) {
        e->ops->port_power_cycle(e->ctx);
    }

    for (int i = 0; i < SLOTS; i++) {
        usbip_engine_slot_t *s = &e->slots[i];

        while (s->wait != NULL) {
            usbip_engine_xfer_t *t = s->wait;

            s->wait = t->next;
            xfer_free(e, t);
        }
        while (s->inflight != NULL) {
            usbip_engine_xfer_t *t = s->inflight;

            s->inflight = t->next;
            if (t->returned) {
                xfer_free(e, t);
            } else {
                t->orphan = true;  // freed when its (canceled) completion arrives
                t->next = NULL;
            }
        }
        memset(s, 0, sizeof(*s));
    }
    usbip_mem_stats_t peaks = e->budget.stats;

    usbip_budget_init(&e->budget);
    e->budget.stats.inflight_peak = peaks.inflight_peak;
    e->budget.stats.waiting_peak = peaks.waiting_peak;
}

bool usbip_engine_active(const usbip_engine_t *e)
{
    return e != NULL && e->active;
}

void usbip_engine_counters(const usbip_engine_t *e, usbip_counters_t *out)
{
    *out = e->counters;
}

void usbip_engine_mem(const usbip_engine_t *e, usbip_mem_stats_t *out)
{
    usbip_budget_stats(&e->budget, out);
}
