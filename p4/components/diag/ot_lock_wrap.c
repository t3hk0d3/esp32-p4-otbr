// Link-time wraps of esp_openthread_lock_acquire / release (-Wl,--wrap, see CMakeLists.txt). Hot path: no allocation,
// no logging; the bookkeeping is in lock_stats.c and runs inside a short spinlock section.
#include <stdbool.h>

#include "esp_openthread_lock.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "diag_priv.h"

bool __real_esp_openthread_lock_acquire(TickType_t block_ticks);
void __real_esp_openthread_lock_release(void);

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static diag_lock_stats_t s_stats; // zero-initialised: no lock held, no figures

bool __wrap_esp_openthread_lock_acquire(TickType_t block_ticks)
{
    const uint64_t start_us = (uint64_t)esp_timer_get_time();
    const bool acquired = __real_esp_openthread_lock_acquire(block_ticks);
    const uint64_t now_us = (uint64_t)esp_timer_get_time();
    const uint64_t wait_us = now_us - start_us;

    portENTER_CRITICAL(&s_mux);
    diag_lock_on_acquire(&s_stats, now_us, wait_us > UINT32_MAX ? UINT32_MAX : (uint32_t)wait_us, acquired);
    portEXIT_CRITICAL(&s_mux);
    return acquired;
}

void __wrap_esp_openthread_lock_release(void)
{
    // Account before the real release: afterwards another task may already own the lock and restart the hold.
    const char *task = pcTaskGetName(NULL);
    const uint64_t now_us = (uint64_t)esp_timer_get_time();

    portENTER_CRITICAL(&s_mux);
    diag_lock_on_release(&s_stats, now_us, task);
    portEXIT_CRITICAL(&s_mux);
    __real_esp_openthread_lock_release();
}

void diag_ot_lock_view(diag_lock_view_t *out)
{
    const uint64_t now_us = (uint64_t)esp_timer_get_time();

    portENTER_CRITICAL(&s_mux);
    diag_lock_view(&s_stats, now_us, out);
    portEXIT_CRITICAL(&s_mux);
}
