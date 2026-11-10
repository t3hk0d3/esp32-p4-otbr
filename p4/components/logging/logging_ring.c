// Transit ring: records are a header plus text, 4-byte aligned, laid out in reservation order. Producers take the
// spinlock only to move `head` and stamp the new header "reserved"; they write the text and commit (a release store
// of the header state) without it. The single consumer reads `head` with acquire, waits on a "reserved" header, and
// frees space by a release store of `tail`. A record that does not fit before the buffer end goes to offset 0; the
// gap left at the end gets a "skip" header, or none when it is smaller than a header (the consumer wraps by itself).

#include <stdint.h>
#include <string.h>
#include "logging_ring.h"

#define RING_ALIGN 4u

enum {
    REC_FREE,
    REC_RESERVED,
    REC_COMMITTED,
    REC_SKIP,
};

typedef struct {
    uint32_t state; /**< REC_*, atomic: reserved under the lock, committed (release) by its producer. */
    uint32_t size;  /**< Whole record, header included; set at reserve. */
    uint32_t uptime_ms;
    uint16_t len;
    uint8_t level;
    char tag[LOGGING_TAG_MAX];
} ring_rec_t;

#define REC_HDR sizeof(ring_rec_t)

_Static_assert(REC_HDR % RING_ALIGN == 0, "record headers must keep records aligned");

static size_t align_up(size_t n)
{
    return (n + RING_ALIGN - 1) & ~(size_t)(RING_ALIGN - 1);
}

static ring_rec_t *rec_at(const logging_ring_t *ring, size_t offset)
{
    return (ring_rec_t *)(ring->buf + offset);
}

void logging_ring_init(logging_ring_t *ring, uint8_t *buf, size_t size)
{
    size_t pad = align_up((uintptr_t) buf) - (uintptr_t) buf;

    ring->buf = buf + pad;
    ring->size = size > pad ? (size - pad) & ~(size_t)(RING_ALIGN - 1) : 0;
    ring->head = 0;
    ring->tail = 0;
    ring->dropped = 0;
    portMUX_INITIALIZE(&ring->lock);
}

bool logging_ring_reserve(logging_ring_t *ring, size_t max_text, logging_ring_slot_t *slot)
{
    size_t need = max_text < ring->size ? align_up(REC_HDR + max_text) : SIZE_MAX;
    size_t pos = SIZE_MAX;
    bool wrap = false;

    portENTER_CRITICAL(&ring->lock);
    size_t head = ring->head;
    size_t tail = __atomic_load_n(&ring->tail, __ATOMIC_ACQUIRE);

    // `head == tail` means empty, so a record may never make them meet: the comparisons with tail are strict.
    if (head >= tail) {
        size_t end_room = ring->size - head;

        if (need < end_room || (need == end_room && tail != 0)) {
            pos = head;
        } else if (need < tail) {
            pos = 0;
            wrap = true;
        }
    } else if (need < tail - head) {
        pos = head;
    }
    if (pos == SIZE_MAX) {
        __atomic_fetch_add(&ring->dropped, 1, __ATOMIC_RELAXED);
        portEXIT_CRITICAL(&ring->lock);
        return false;
    }
    if (wrap && ring->size - head >= REC_HDR) {
        __atomic_store_n(&rec_at(ring, head)->state, REC_SKIP, __ATOMIC_RELAXED);
    }
    ring_rec_t *rec = rec_at(ring, pos);

    rec->size = need;
    __atomic_store_n(&rec->state, REC_RESERVED, __ATOMIC_RELAXED);
    // Publishes the headers above to the consumer.
    __atomic_store_n(&ring->head, pos + need == ring->size ? 0 : pos + need, __ATOMIC_RELEASE);
    portEXIT_CRITICAL(&ring->lock);

    slot->offset = pos;
    slot->text = (char *)(ring->buf + pos + REC_HDR);
    slot->cap = need - REC_HDR;
    return true;
}

void logging_ring_commit(logging_ring_t *ring, logging_ring_slot_t *slot, const logging_line_t *meta,
                         size_t text_used)
{
    ring_rec_t *rec = rec_at(ring, slot->offset);

    rec->uptime_ms = meta->uptime_ms;
    rec->level = (uint8_t) meta->level;
    strlcpy(rec->tag, meta->tag, sizeof(rec->tag));
    rec->len = (uint16_t)(text_used < slot->cap ? text_used : slot->cap);
    // Publishes the fields above and the text to the consumer.
    __atomic_store_n(&rec->state, REC_COMMITTED, __ATOMIC_RELEASE);
}

bool logging_ring_pop(logging_ring_t *ring, logging_line_t *out, char *text, size_t cap)
{
    size_t tail = ring->tail; // only the consumer writes it

    for (;;) {
        size_t head = __atomic_load_n(&ring->head, __ATOMIC_ACQUIRE);

        if (tail == head) {
            return false;
        }
        if (ring->size - tail < REC_HDR) {
            tail = 0; // end gap too small for a skip header
            __atomic_store_n(&ring->tail, tail, __ATOMIC_RELEASE);
            continue;
        }
        ring_rec_t *rec = rec_at(ring, tail);
        uint32_t state = __atomic_load_n(&rec->state, __ATOMIC_ACQUIRE);

        if (state == REC_SKIP) {
            __atomic_store_n(&rec->state, REC_FREE, __ATOMIC_RELAXED);
            tail = 0;
            __atomic_store_n(&ring->tail, tail, __ATOMIC_RELEASE);
            continue;
        }
        if (state != REC_COMMITTED) {
            return false; // its producer is still writing; later records wait behind it
        }
        size_t n = rec->len < cap ? rec->len : cap;

        memcpy(text, ring->buf + tail + REC_HDR, n);
        out->uptime_ms = rec->uptime_ms;
        out->level = (esp_log_level_t) rec->level;
        strlcpy(out->tag, rec->tag, sizeof(out->tag));
        out->len = (uint16_t) n;
        out->text = text;
        tail += rec->size;
        __atomic_store_n(&rec->state, REC_FREE, __ATOMIC_RELAXED);
        // Hands the space back to the producers, after the copy above.
        __atomic_store_n(&ring->tail, tail == ring->size ? 0 : tail, __ATOMIC_RELEASE);
        return true;
    }
}

uint32_t logging_ring_dropped(const logging_ring_t *ring)
{
    return __atomic_load_n(&ring->dropped, __ATOMIC_RELAXED);
}
