#pragma once

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The single-producer, single-consumer byte ring that carries ziggurat's log lines from ziggurat_platform_log (any
 * context, interrupts possibly masked: it only copies) to the drain task. A record is 3 header bytes (level, length
 * low, length high) and the text. `head` and `tail` are free-running uint32 counters (index = counter & (size - 1)).
 * Without a buffer (not allocated yet) every put is dropped and counted.
 */
#define ZIGBEE_ZLOG_LINE_MAX 512

typedef struct {
    uint8_t *buf;  // NULL until zigbee_zlog_ring_init
    uint32_t size; // a power of two
    atomic_uint head; // written by the producer only
    atomic_uint tail; // written by the consumer only
    atomic_uint dropped;
} zigbee_zlog_ring_t;

/** Sets the buffer (`size` a power of two) and empties the ring. Before the producer runs. buf NULL: all puts drop. */
void zigbee_zlog_ring_init(zigbee_zlog_ring_t *r, uint8_t *buf, uint32_t size);

/** Producer: copies one record (len clamped to ZIGBEE_ZLOG_LINE_MAX). No room or no buffer: dropped + 1. Never blocks. */
void zigbee_zlog_ring_put(zigbee_zlog_ring_t *r, uint8_t level, const uint8_t *msg, size_t len);

/**
 * Consumer: takes the oldest record. `out` has room for ZIGBEE_ZLOG_LINE_MAX + 1 bytes; the text is NUL-terminated there.
 * false when the ring is empty.
 */
bool zigbee_zlog_ring_get(zigbee_zlog_ring_t *r, uint8_t *level, char *out, size_t *len);

/** Lines dropped since init. */
uint32_t zigbee_zlog_ring_dropped(zigbee_zlog_ring_t *r);

#ifdef __cplusplus
}
#endif
