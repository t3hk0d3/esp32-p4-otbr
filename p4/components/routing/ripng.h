#pragma once

// RIPng codec and scheduler (spec section 5, RFC 2080): an announce-only sender. The codec encodes responses (one
// 20-byte RTE per /64, route tag 0, prefix length 64, no next-hop RTE: the next-hop is the source address), classifies
// received datagrams and answers requests. The scheduler decides when regular and triggered updates go out and how long
// a withdrawn prefix keeps metric 16. Pure, host-tested against scapy-built vectors; it makes no socket calls (the
// routing task sends what it returns to ff02::9 port 521).
//
// Times are uint32_t milliseconds compared wrap-safe (deadlines lie well within 2^31 ms of now).

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "routing_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ROUTING_RIPNG_PORT 521
#define ROUTING_RIPNG_METRIC 1
#define ROUTING_RIPNG_INFINITY 16
#define ROUTING_RIPNG_MAX_PAYLOAD 1232 /**< IPv6 minimum MTU 1280 - IPv6 header 40 - UDP header 8 */
#define ROUTING_RIPNG_HDR_LEN 4
#define ROUTING_RIPNG_RTE_LEN 20

#define ROUTING_RIPNG_UPDATE_MS 30000       /**< regular update interval ... */
#define ROUTING_RIPNG_UPDATE_JITTER_MS 5000 /**< ... plus a random 0..5000 ms, drawn per update */
#define ROUTING_RIPNG_TRIGGER_GAP_MS 5000   /**< minimum gap between triggered updates */
#define ROUTING_RIPNG_WITHDRAW_UPDATES 4    /**< regular updates that carry a withdrawn prefix after its trigger */
#define ROUTING_RIPNG_MAX_WITHDRAWN (2 * ROUTING_MAX_ANNOUNCED) /**< withdrawn prefixes still sent at metric 16 */
/** The most entries the scheduler returns at once: out_cap for poll and stop. */
#define ROUTING_RIPNG_MAX_RTES (ROUTING_MAX_ANNOUNCED + ROUTING_RIPNG_MAX_WITHDRAWN)

/** One route entry: a /64 and its metric (ROUTING_RIPNG_METRIC or ROUTING_RIPNG_INFINITY). */
typedef struct {
    routing_prefix64_t prefix;
    uint8_t metric;
} routing_ripng_rte_t;

typedef enum {
    ROUTING_RIPNG_INVALID,      /**< drop: shorter than the header, version not 1, command not 1/2, or a partial RTE */
    ROUTING_RIPNG_RESPONSE,     /**< from a neighbour: counted and dropped */
    ROUTING_RIPNG_REQUEST_ALL,  /**< exactly one RTE ::/0, prefix length 0, metric 16 */
    ROUTING_RIPNG_REQUEST_SOME, /**< any other request (zero or more RTEs) */
} routing_ripng_kind_t;

/**
 * Writes one response message (command 2, version 1) carrying as many of `rtes` as fit in `cap`, in order, and sets
 * `*consumed` to how many it took. Call again with `rtes + *consumed` for the rest. Returns the message length, or 0
 * (consumed 0, nothing written) when `n` is 0 or not even one RTE fits.
 */
size_t routing_ripng_encode_response(uint8_t *buf, size_t cap, const routing_ripng_rte_t *rtes, size_t n,
                                     size_t *consumed);

/**
 * Writes a response message without RTEs (command 2, version 1, the 4-byte header only): the answer to a whole-table
 * request while nothing is announced (RFC 2080 section 2.4.1 answers every request). Returns 4, or 0 (nothing written)
 * when `cap` is below 4.
 */
size_t routing_ripng_encode_empty_response(uint8_t *buf, size_t cap);

/** Classifies a received datagram (`len` bytes of UDP payload). */
routing_ripng_kind_t routing_ripng_classify(const uint8_t *buf, size_t len);

/**
 * The answer to a whole-table request: every `announced` prefix at metric 1 (at most `out_cap`), returning how many.
 * Encode it with routing_ripng_encode_response, or routing_ripng_encode_empty_response when it returns 0.
 */
size_t routing_ripng_table(const routing_prefix_set_t *announced, routing_ripng_rte_t *out, size_t out_cap);

/**
 * The answer to a request for specific prefixes (RFC 2080 section 2.4.1): writes the request back as a response
 * (command 2, version 1) with every RTE unchanged (all 16 prefix bytes, route tag, prefix length) except its metric:
 * 1 when the RTE is exactly an announced /64 (prefix length 64, first 8 bytes announced, last 8 bytes zero), 16
 * otherwise. Returns the length written (`len`), or 0 without writing anything when the request is not
 * ROUTING_RIPNG_REQUEST_SOME, has no RTEs, or does not fit in `cap`.
 */
size_t routing_ripng_encode_answer(const uint8_t *req, size_t len, const routing_prefix_set_t *announced, uint8_t *buf,
                                   size_t cap);

/** A prefix withdrawn from the announcement set, still sent at metric 16. */
typedef struct {
    routing_prefix64_t prefix;
    uint8_t left;  /**< regular updates that still carry it */
    bool changed;  /**< not yet sent in an update since it was withdrawn */
} routing_ripng_withdrawn_t;

/** Scheduler state; the caller owns the storage. Fields are private to ripng.c. */
typedef struct {
    bool running;
    uint32_t (*rand32)(void);
    routing_prefix_set_t announced;
    bool announced_changed[ROUTING_MAX_ANNOUNCED]; /**< added since the last update that carried it */
    routing_ripng_withdrawn_t withdrawn[ROUTING_RIPNG_MAX_WITHDRAWN];
    uint8_t withdrawn_count;
    uint32_t regular_at;
    bool trigger_pending;
    uint32_t trigger_at;
    bool trigger_gap; /**< a triggered update went out less than 5000 ms ago (cleared once that has passed) */
    uint32_t last_trigger_ms;
} routing_ripng_sched_t;

/**
 * Starts (or restarts) the scheduler with announcement set `set`: a full update is due at once (the next poll), then
 * one every ROUTING_RIPNG_UPDATE_MS + rand32() % 5001 ms. `rand32` is required.
 */
void routing_ripng_sched_start(routing_ripng_sched_t *s, const routing_prefix_set_t *set, uint32_t now_ms,
                               uint32_t (*rand32)(void));

/**
 * The announcement set is now `set`. Added prefixes are sent at metric 1 and removed ones at metric 16 in a triggered
 * update due at once, or at the previous triggered update + 5000 ms if that is later; changes until then merge into
 * it. A removed prefix is then also carried at metric 16 by the next 4 regular updates. A re-announced prefix goes back
 * to metric 1 at once. When the withdrawn list is full, the entry closest to expiry gives way. Ignored when stopped.
 */
void routing_ripng_sched_set_changed(routing_ripng_sched_t *s, const routing_prefix_set_t *set, uint32_t now_ms);

/**
 * The entries to send now (at most `out_cap`; pass ROUTING_RIPNG_MAX_RTES), 0 for nothing. A regular update carries
 * every announced prefix at metric 1 and every withdrawn one at 16; it replaces a triggered update still pending. A
 * triggered update carries only the changed entries. Returns 0 while stopped.
 */
size_t routing_ripng_sched_poll(routing_ripng_sched_t *s, uint32_t now_ms, routing_ripng_rte_t *out, size_t out_cap);

/**
 * Stops the scheduler: returns every announced prefix and every still-withdrawn one at metric 16, once. Afterwards
 * poll returns 0 and next_deadline_ms UINT32_MAX until the next start. Returns 0 when already stopped.
 */
size_t routing_ripng_sched_stop(routing_ripng_sched_t *s, routing_ripng_rte_t *out, size_t out_cap);

/**
 * When to poll next (the earliest of the regular and a pending triggered update), or UINT32_MAX when stopped.
 * UINT32_MAX is never a real deadline: an update that would fall due at UINT32_MAX falls due 1 ms later, at 0.
 */
uint32_t routing_ripng_sched_next_deadline_ms(const routing_ripng_sched_t *s);

#ifdef __cplusplus
}
#endif
