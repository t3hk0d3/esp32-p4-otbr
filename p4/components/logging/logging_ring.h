#pragma once

// Transit ring of the logging component: log lines on their way from the capture (any task, both cores) to the drain
// task. Many producers reserve a record under a spinlock (index math only), write the text outside it and commit; one
// consumer pops records in reservation order. A full ring drops the line and counts it. Not for ISRs.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "logging.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t *buf;
    size_t size;      /**< Usable bytes: a multiple of 4. */
    size_t head;      /**< Next reservation; written under `lock`, read by the consumer (acquire). */
    size_t tail;      /**< Oldest record; written by the consumer (release), read under `lock`. */
    uint32_t dropped; /**< Reservations refused for lack of space. */
    portMUX_TYPE lock;
} logging_ring_t;

/** A reserved record: the producer writes up to `cap` bytes of text at `text`, then commits. */
typedef struct {
    size_t offset; /**< Record header offset in the ring. */
    char *text;
    size_t cap;
} logging_ring_slot_t;

/** Use `size` bytes at `buf` (any alignment; a few bytes may go unused to align records). Empties the ring. */
void logging_ring_init(logging_ring_t *ring, uint8_t *buf, size_t size);

/**
 * Reserve a record for up to `max_text` bytes of text. False when the ring has no room (the dropped counter goes up,
 * nothing is written). Never blocks; the record stays invisible to the consumer, and holds back every later record,
 * until it is committed.
 */
bool logging_ring_reserve(logging_ring_t *ring, size_t max_text, logging_ring_slot_t *slot);

/**
 * Publish a reserved record: level, tag and uptime from `meta` (its len and text are ignored), `text_used` bytes of
 * text already written at `slot->text` (clamped to `slot->cap`).
 */
void logging_ring_commit(logging_ring_t *ring, logging_ring_slot_t *slot, const logging_line_t *meta,
                         size_t text_used);

/**
 * Take the oldest record (single consumer). False when the ring is empty or its oldest record is not committed yet.
 * Copies at most `cap` bytes of text into `text` (not NUL-terminated); `out->text` points at `text`.
 */
bool logging_ring_pop(logging_ring_t *ring, logging_line_t *out, char *text, size_t cap);

/** Reservations refused for lack of space since init. */
uint32_t logging_ring_dropped(const logging_ring_t *ring);

#ifdef __cplusplus
}
#endif
