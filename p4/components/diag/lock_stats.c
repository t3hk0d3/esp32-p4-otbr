#include "lock_stats.h"

#include <string.h>

#define US_PER_S 1000000ULL

static diag_lock_bucket_t *bucket_for(diag_lock_stats_t *s, uint64_t now_us)
{
    const uint32_t sec = (uint32_t)(now_us / US_PER_S);
    diag_lock_bucket_t *b = &s->buckets[sec % DIAG_LOCK_BUCKETS];

    if (b->sec != sec) {
        memset(b, 0, sizeof(*b));
        b->sec = sec;
    }
    return b;
}

static uint32_t clamp_us(uint64_t value)
{
    return value > UINT32_MAX ? UINT32_MAX : (uint32_t)value;
}

static void set_name(char *dst, const char *src)
{
    strncpy(dst, src ? src : "", DIAG_LOCK_NAME_MAX - 1);
    dst[DIAG_LOCK_NAME_MAX - 1] = '\0';
}

void diag_lock_on_acquire(diag_lock_stats_t *s, uint64_t now_us, uint32_t wait_us, bool acquired)
{
    diag_lock_bucket_t *b = bucket_for(s, now_us);

    s->wait_us_total += wait_us;
    if (wait_us > s->max_wait_us) {
        s->max_wait_us = wait_us;
    }
    if (wait_us > b->max_wait_us) {
        b->max_wait_us = wait_us;
    }
    if (!acquired) {
        return;
    }
    if (s->depth++ == 0) {
        s->hold_start_us = now_us;
        s->acquisitions++;
        b->acquisitions++;
    }
}

void diag_lock_on_release(diag_lock_stats_t *s, uint64_t now_us, const char *task)
{
    if (s->depth == 0 || --s->depth != 0) {
        return;
    }
    const uint32_t hold = clamp_us(now_us - s->hold_start_us);
    diag_lock_bucket_t *b = bucket_for(s, now_us);

    s->hold_us_total += now_us - s->hold_start_us;
    if (hold > s->max_hold_us) {
        s->max_hold_us = hold;
        set_name(s->hold_task, task);
    }
    if (hold > b->max_hold_us) {
        b->max_hold_us = hold;
        set_name(b->hold_task, task);
    }
}

void diag_lock_view(const diag_lock_stats_t *s, uint64_t now_us, diag_lock_view_t *out)
{
    const uint32_t now_sec = (uint32_t)(now_us / US_PER_S);

    memset(out, 0, sizeof(*out));
    out->since_boot.acquisitions = s->acquisitions;
    out->hold_us_total = s->hold_us_total;
    out->wait_us_total = s->wait_us_total;
    out->since_boot.max_hold_us = s->max_hold_us;
    out->since_boot.max_wait_us = s->max_wait_us;
    set_name(out->since_boot.hold_task, s->hold_task);
    for (int i = 0; i < DIAG_LOCK_BUCKETS; i++) {
        const diag_lock_bucket_t *b = &s->buckets[i];

        if (b->sec > now_sec || now_sec - b->sec >= DIAG_LOCK_BUCKETS) {
            continue;
        }
        out->last_60s.acquisitions += b->acquisitions;
        if (b->max_wait_us > out->last_60s.max_wait_us) {
            out->last_60s.max_wait_us = b->max_wait_us;
        }
        if (b->max_hold_us > out->last_60s.max_hold_us) {
            out->last_60s.max_hold_us = b->max_hold_us;
            set_name(out->last_60s.hold_task, b->hold_task);
        }
    }
}
