#pragma once

// Capture of the C6 console: a byte ring (served at GET /device/radio/log) and complete lines (forwarded to the
// P4 log). Pure: jumper.c owns the UART, the lock and the storage.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LOG_RING_LINE_MAX 200 /* longer lines are forwarded in pieces of this size */

typedef void (*log_ring_line_cb_t)(void *ctx, const char *line);

typedef struct {
    uint8_t *storage;
    size_t cap;
    size_t head; /**< next write position */
    size_t len;  /**< bytes held, at most cap */
    char line[LOG_RING_LINE_MAX + 1];
    size_t line_len;
    int escape;  /**< 0 text, 1 after ESC, 2 inside a CSI sequence */
    log_ring_line_cb_t on_line;
    void *ctx;
} log_ring_t;

/** `storage` (cap bytes) must outlive the ring. on_line may be NULL. */
void log_ring_init(log_ring_t *ring, uint8_t *storage, size_t cap, log_ring_line_cb_t on_line, void *ctx);

/**
 * Append raw bytes to the ring (the oldest are dropped when full) and hand every completed line to on_line: CR and
 * ANSI colour sequences removed, other control characters dropped, empty lines skipped.
 */
void log_ring_push(log_ring_t *ring, const uint8_t *data, size_t len);

/** Copy the newest min(len, cap) bytes, oldest first, into out (no terminator). Returns the count. */
size_t log_ring_read(const log_ring_t *ring, char *out, size_t cap);

/** True when the line is the C6 ROM boot banner ("ESP-ROM:..."). */
bool log_ring_is_rom_banner(const char *line);

#ifdef __cplusplus
}
#endif
