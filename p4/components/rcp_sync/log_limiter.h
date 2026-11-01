#pragma once

// Rate limit for the C6 console lines forwarded to the P4 log (a C6 boot loop prints without pause and would drown the P4
// console). Token bucket with a burst of one second's worth, plus a summary count of what was dropped. Pure: the caller
// supplies the time and does the logging; the 16 KB byte ring (GET /device/radio/log) is not limited.

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LOG_LIMITER_RATE_PER_S    20u   /* lines per second forwarded in the long run, and the burst size */
#define LOG_LIMITER_SUMMARY_MS    1000u /* at most one "n lines suppressed" summary per this interval */

typedef struct {
    uint32_t rate_per_s;
    uint32_t tokens_milli;   /**< 1000 = one line */
    uint32_t last_ms;
    bool started;
    uint32_t suppressed;     /**< dropped since the last summary */
    uint32_t window_start_ms; /**< time of the first drop since the last summary */
} log_limiter_t;

/** Full bucket. `rate_per_s` lines per second (and burst); 0 is treated as LOG_LIMITER_RATE_PER_S. */
void log_limiter_init(log_limiter_t *l, uint32_t rate_per_s);

/** True: forward this line. False: it is dropped and counted. `now_ms` is any monotonic millisecond clock (wraps are fine). */
bool log_limiter_allow(log_limiter_t *l, uint32_t now_ms);

/**
 * Number of lines dropped since the previous summary, once at least LOG_LIMITER_SUMMARY_MS have passed since the first of
 * them was dropped; 0 otherwise (nothing dropped, or too early). Resets the count. Poll it when idle as well, so that the
 * summary of a flood that stopped is still printed.
 */
uint32_t log_limiter_take_summary(log_limiter_t *l, uint32_t now_ms);

#ifdef __cplusplus
}
#endif
