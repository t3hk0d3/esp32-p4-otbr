#pragma once

// Small helpers shared by the pure routing modules: /64 comparison and the wrap-safe millisecond clock.

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "routing_types.h"

static inline bool routing_prefix64_eq(const routing_prefix64_t *a, const routing_prefix64_t *b)
{
    return memcmp(a->b, b->b, sizeof(a->b)) == 0;
}

/* Wrap-safe: true once `now` has reached `at` (deadlines lie well within 2^31 ms of now). */
static inline bool routing_due(uint32_t now, uint32_t at)
{
    return (int32_t)(now - at) >= 0;
}

/* now + delay, moved 1 ms later when it would equal UINT32_MAX, which the next-deadline getters reserve for "none". */
static inline uint32_t routing_deadline(uint32_t now, uint32_t delay_ms)
{
    uint32_t at = now + delay_ms;

    return at == UINT32_MAX ? 0 : at;
}
