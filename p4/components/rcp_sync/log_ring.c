#include <string.h>
#include "log_ring.h"

#define ESC          0x1b
#define BANNER       "ESP-ROM:"
#define CSI_FINAL_LO 0x40 /* a CSI sequence ends with a byte in 0x40..0x7e */
#define CSI_FINAL_HI 0x7e

void log_ring_init(log_ring_t *ring, uint8_t *storage, size_t cap, log_ring_line_cb_t on_line, void *ctx)
{
    memset(ring, 0, sizeof(*ring));
    ring->storage = storage;
    ring->cap = cap;
    ring->on_line = on_line;
    ring->ctx = ctx;
}

static void flush_line(log_ring_t *ring)
{
    if (ring->line_len > 0 && ring->on_line) {
        ring->line[ring->line_len] = '\0';
        ring->on_line(ring->ctx, ring->line);
    }
    ring->line_len = 0;
}

// Text of one byte for the forwarded line: ANSI sequences and control characters are dropped.
static void line_byte(log_ring_t *ring, uint8_t c)
{
    if (ring->escape == 1) {
        ring->escape = c == '[' ? 2 : 0;
        return;
    }
    if (ring->escape == 2) {
        if (c >= CSI_FINAL_LO && c <= CSI_FINAL_HI) {
            ring->escape = 0;
        }
        return;
    }
    if (c == ESC) {
        ring->escape = 1;
    } else if (c == '\n') {
        flush_line(ring);
    } else if (c == '\t' || c >= ' ') {
        ring->line[ring->line_len++] = (char)c;
        if (ring->line_len == LOG_RING_LINE_MAX) {
            flush_line(ring);
        }
    }
}

void log_ring_push(log_ring_t *ring, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (ring->cap > 0) {
            ring->storage[ring->head] = data[i];
            ring->head = (ring->head + 1) % ring->cap;
            if (ring->len < ring->cap) {
                ring->len++;
            }
        }
        line_byte(ring, data[i]);
    }
}

size_t log_ring_read(const log_ring_t *ring, char *out, size_t cap)
{
    const size_t n = ring->len < cap ? ring->len : cap;
    size_t pos = (ring->head + ring->cap - n) % (ring->cap ? ring->cap : 1);

    for (size_t i = 0; i < n; i++) {
        out[i] = (char)ring->storage[pos];
        pos = (pos + 1) % ring->cap;
    }
    return n;
}

bool log_ring_is_rom_banner(const char *line)
{
    return line && strstr(line, BANNER) != NULL;
}
