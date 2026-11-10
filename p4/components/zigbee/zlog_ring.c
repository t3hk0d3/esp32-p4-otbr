#include "zigbee_zlog_ring.h"

#include <stdbool.h>
#include <string.h>

#define HDR 3

// Copies n bytes into / out of the ring at counter `at`, in at most two pieces around the end.
static void ring_put(const zigbee_zlog_ring_t *r, uint32_t at, const void *src, size_t n)
{
    const uint32_t i = at & (r->size - 1);
    const size_t first = n < r->size - i ? n : r->size - i;

    memcpy(r->buf + i, src, first);
    memcpy(r->buf, (const uint8_t *)src + first, n - first);
}

static void ring_get(const zigbee_zlog_ring_t *r, uint32_t at, void *dst, size_t n)
{
    const uint32_t i = at & (r->size - 1);
    const size_t first = n < r->size - i ? n : r->size - i;

    memcpy(dst, r->buf + i, first);
    memcpy((uint8_t *)dst + first, r->buf, n - first);
}

void zigbee_zlog_ring_init(zigbee_zlog_ring_t *r, uint8_t *buf, uint32_t size)
{
    r->buf = buf;
    r->size = size;
    atomic_store(&r->head, 0);
    atomic_store(&r->tail, 0);
    atomic_store(&r->dropped, 0);
}

void zigbee_zlog_ring_put(zigbee_zlog_ring_t *r, uint8_t level, const uint8_t *msg, size_t len)
{
    if (len > ZIGBEE_ZLOG_LINE_MAX) {
        len = ZIGBEE_ZLOG_LINE_MAX;
    }
    const uint32_t head = atomic_load_explicit(&r->head, memory_order_relaxed);
    const uint32_t tail = atomic_load_explicit(&r->tail, memory_order_acquire);
    const size_t need = HDR + len;

    if (r->buf == NULL || r->size - (head - tail) < need) {
        atomic_fetch_add_explicit(&r->dropped, 1, memory_order_relaxed);
        return;
    }
    const uint8_t hdr[HDR] = {level, (uint8_t)(len & 0xff), (uint8_t)(len >> 8)};

    ring_put(r, head, hdr, HDR);
    ring_put(r, head + HDR, msg, len);
    atomic_store_explicit(&r->head, head + need, memory_order_release);
}

bool zigbee_zlog_ring_get(zigbee_zlog_ring_t *r, uint8_t *level, char *out, size_t *len)
{
    const uint32_t tail = atomic_load_explicit(&r->tail, memory_order_relaxed);
    const uint32_t head = atomic_load_explicit(&r->head, memory_order_acquire);

    if (r->buf == NULL || tail == head) {
        return false;
    }
    uint8_t hdr[HDR];

    ring_get(r, tail, hdr, HDR);
    const size_t n = hdr[1] | ((size_t)hdr[2] << 8);

    ring_get(r, tail + HDR, out, n);
    out[n] = '\0';
    *level = hdr[0];
    *len = n;
    atomic_store_explicit(&r->tail, tail + HDR + (uint32_t)n, memory_order_release);
    return true;
}

uint32_t zigbee_zlog_ring_dropped(zigbee_zlog_ring_t *r)
{
    return atomic_load_explicit(&r->dropped, memory_order_relaxed);
}
