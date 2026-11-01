#include "log_limiter.h"

#define TOKEN 1000u

void log_limiter_init(log_limiter_t *l, uint32_t rate_per_s)
{
    l->rate_per_s = rate_per_s ? rate_per_s : LOG_LIMITER_RATE_PER_S;
    l->tokens_milli = l->rate_per_s * TOKEN;
    l->last_ms = 0;
    l->started = false;
    l->suppressed = 0;
    l->window_start_ms = 0;
}

static void refill(log_limiter_t *l, uint32_t now_ms)
{
    const uint32_t full = l->rate_per_s * TOKEN;

    if (l->started) {
        const uint32_t elapsed = now_ms - l->last_ms; // wrap-safe
        // elapsed * rate = milli-tokens (rate lines/s = rate milli-tokens/ms); saturate before it can overflow
        l->tokens_milli = elapsed >= 1000u ? full : (l->tokens_milli + elapsed * l->rate_per_s > full ? full : l->tokens_milli + elapsed * l->rate_per_s);
    }
    l->started = true;
    l->last_ms = now_ms;
}

bool log_limiter_allow(log_limiter_t *l, uint32_t now_ms)
{
    refill(l, now_ms);
    if (l->tokens_milli >= TOKEN) {
        l->tokens_milli -= TOKEN;
        return true;
    }
    if (l->suppressed == 0) {
        l->window_start_ms = now_ms;
    }
    l->suppressed++;
    return false;
}

uint32_t log_limiter_take_summary(log_limiter_t *l, uint32_t now_ms)
{
    if (l->suppressed == 0 || now_ms - l->window_start_ms < LOG_LIMITER_SUMMARY_MS) {
        return 0;
    }
    const uint32_t n = l->suppressed;

    l->suppressed = 0;
    return n;
}
