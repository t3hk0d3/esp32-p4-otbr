// Capture of the logging component (target only): every ESP-IDF log call passes through here, by the linker's
// --wrap=esp_log and --wrap=esp_log_va (CMakeLists.txt). The ESP_LOGx macros call esp_log; esp_log_write and
// esp_log_writev (another translation unit of ESP-IDF's log component) call esp_log_va. esp_log's own call to
// esp_log_va inside log.c cannot be wrapped, so __wrap_esp_log never calls __real_esp_log: it goes straight to
// __wrap_esp_log_va.
//
// A line at or below the UART level goes, unchanged, through __real_esp_log_va: ESP-IDF's own synchronous path, same
// bytes as before. A line some other channel wants (logging_capture_route) is formatted into the transit ring and the
// drain task is notified. Never blocks beyond what ESP-IDF's own path does (its tag-level lookup waits at most 10 ms
// for the log mutex); never takes the settings mutex (lines are logged while it is held): the levels come from
// logging_levels(), one atomic load.
//
// Constrained contexts pass straight through to ESP-IDF, as does the time before logging_start: an ISR, the flash
// cache disabled, the scheduler not running or suspended (esp_log_util_is_constrained, ESP-IDF's own test). With
// CONFIG_LOG_IN_IRAM ESP-IDF's log path lives in IRAM, so the two wrappers do too (IRAM_ATTR) and touch nothing
// outside IRAM/DRAM before that test: a disabled ESP_LOGx in IRAM-safe code keeps working while the cache is off.

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_private/log_level.h" // esp_log_level_get_timeout: the bounded lookup esp_log_va itself uses
#include "esp_private/log_util.h"  // esp_log_util_is_constrained: ISR, cache off, scheduler not running
#include "logging_core.h"
#include "logging_priv.h"
#include "logging_ring.h"
#include "logging_text.h"

static const char *TAG = "logging";

#define RING_SIZE (16 * 1024) /**< Internal RAM: every task writes it, the drain reads it. */
#define GUARDS_MAX 8
/** Longest formatted line kept: the ESP-IDF prefix (colour, letter, uptime, tag), a cut body and the colour reset. */
#define FORMAT_MAX (LOGGING_LINE_MAX + 128)

void __real_esp_log_va(esp_log_config_t config, const char *tag, const char *format, va_list args);
void __wrap_esp_log_va(esp_log_config_t config, const char *tag, const char *format, va_list args);
void __wrap_esp_log(esp_log_config_t config, const char *tag, const char *format, ...);

typedef struct {
    TaskHandle_t owner; /**< Atomic: claimed by CAS from NULL, cleared by its owner; read by every log call. */
    uint32_t depth;     /**< Owner only. */
} guard_t;

static guard_t s_guards[GUARDS_MAX];
static logging_ring_t s_ring;
static uint8_t *s_ring_buf;
static TaskHandle_t s_drain;
static bool s_ready; // atomic: s_ring and s_drain are set up (release / acquire); DRAM, read with the cache off
// Atomic counters: lines ESP-IDF wrote to the UART (as far as the capture can tell), and lines the full ring refused,
// per channel that wanted them.
static uint32_t s_uart_sent;
static uint32_t s_ring_dropped[LOGGING_CH_COUNT];
static uint32_t s_live_dropped;

static guard_t *find_guard(TaskHandle_t task)
{
    for (int i = 0; i < GUARDS_MAX; i++) {
        if (__atomic_load_n(&s_guards[i].owner, __ATOMIC_ACQUIRE) == task) {
            return &s_guards[i];
        }
    }
    return NULL;
}

void logging_sink_enter(void)
{
    TaskHandle_t me = xTaskGetCurrentTaskHandle();
    guard_t *g = find_guard(me);

    if (g) {
        g->depth++;
        return;
    }
    for (int i = 0; i < GUARDS_MAX; i++) {
        TaskHandle_t expected = NULL;

        if (__atomic_compare_exchange_n(&s_guards[i].owner, &expected, me, false, __ATOMIC_ACQ_REL,
                                        __ATOMIC_RELAXED)) {
            s_guards[i].depth = 1;
            return;
        }
    }
    // Full: ignored (documented in logging.h). Not logged: the caller is about to send a line.
}

void logging_sink_leave(void)
{
    guard_t *g = find_guard(xTaskGetCurrentTaskHandle());

    if (g && --g->depth == 0) {
        __atomic_store_n(&g->owner, NULL, __ATOMIC_RELEASE);
    }
}

static void count_ring_drop(esp_log_level_t level, logging_levels_t lv)
{
    unsigned want = logging_core_wanting(level, lv);

    for (int ch = 0; ch < LOGGING_CH_COUNT; ch++) {
        if (want & (1u << ch)) {
            __atomic_fetch_add(&s_ring_dropped[ch], 1, __ATOMIC_RELAXED);
        }
    }
    if (want & LOGGING_WANT_LIVE) {
        __atomic_fetch_add(&s_live_dropped, 1, __ATOMIC_RELAXED);
    }
}

/** Format the line into one ring record (its body only) and wake the drain; a full ring drops it and counts it. */
static void capture(esp_log_level_t level, const char *tag, const char *format, va_list args, logging_levels_t lv)
{
    logging_line_t meta = {.uptime_ms = esp_log_timestamp(), .level = level};
    logging_ring_slot_t slot;
    const char *body;
    va_list ap;
    int n;

    // Measured first, so the record takes only the room the line needs.
    va_copy(ap, args);
    n = vsnprintf(NULL, 0, format, ap);
    va_end(ap);
    if (n <= 0) {
        return;
    }
    size_t want = (size_t) n < FORMAT_MAX ? (size_t) n : FORMAT_MAX;

    strlcpy(meta.tag, tag ? tag : "", sizeof(meta.tag));
    if (!logging_ring_reserve(&s_ring, want + 1, &slot)) {
        count_ring_drop(level, lv);
        return;
    }
    // Reserved: until the commit this record holds back every later one, so only the formatting and the body strip
    // run here; nothing that can block or log.
    va_copy(ap, args);
    n = vsnprintf(slot.text, want + 1, format, ap);
    va_end(ap);
    size_t got = n < 0 ? 0 : ((size_t) n < want ? (size_t) n : want);
    size_t len = logging_text_body(slot.text, got, &body);
    bool cut = (size_t) n > want || len > LOGGING_LINE_MAX;
    size_t keep = len < LOGGING_LINE_MAX ? len : LOGGING_LINE_MAX;

    memmove(slot.text, body, keep);
    if (cut) {
        keep = logging_text_truncate(slot.text, keep + 1, keep);
    }
    logging_ring_commit(&s_ring, &slot, &meta, keep);
    xTaskNotifyGive(s_drain);
}

/** The unconstrained path (flash code): route the line to the UART and/or the ring. */
static __attribute__((noinline)) void route_line(esp_log_config_t config, const char *tag, const char *format,
                                                 va_list args)
{
    esp_log_level_t level = config.opts.log_level;
    logging_levels_t lv = logging_levels();
    esp_log_level_t uart = (esp_log_level_t) lv.channel[LOGGING_CH_UART];
    esp_log_level_t nonuart = logging_core_nonuart_max(lv);
    esp_log_level_t tag_level = ESP_LOG_NONE;
    bool guard = false;
    unsigned route;

    if (level != ESP_LOG_NONE && (level <= uart || level <= nonuart)) {
        guard = level <= nonuart && find_guard(xTaskGetCurrentTaskHandle()) != NULL;
        tag_level = esp_log_level_get_timeout(tag);
    }
    route = logging_capture_route(level, uart, nonuart, tag_level, guard, false);
    if (route & LOGGING_ROUTE_UART) {
        va_list ap;

        va_copy(ap, args);
        __real_esp_log_va(config, tag, format, ap);
        va_end(ap);
        if (level <= tag_level) {
            __atomic_fetch_add(&s_uart_sent, 1, __ATOMIC_RELAXED);
        }
    }
    if (route & LOGGING_ROUTE_RING) {
        capture(level, tag, format, args, lv);
    }
}

void IRAM_ATTR __wrap_esp_log_va(esp_log_config_t config, const char *tag, const char *format, va_list args)
{
    // Nothing outside IRAM/DRAM before this test (see the top of the file).
    if (!__atomic_load_n(&s_ready, __ATOMIC_ACQUIRE) || esp_log_util_is_constrained()) {
        __real_esp_log_va(config, tag, format, args);
        return;
    }
    route_line(config, tag, format, args);
}

void IRAM_ATTR __wrap_esp_log(esp_log_config_t config, const char *tag, const char *format, ...)
{
    va_list args;

    va_start(args, format);
    __wrap_esp_log_va(config, tag, format, args);
    va_end(args);
}

esp_err_t logging_start(void)
{
    if (__atomic_load_n(&s_ready, __ATOMIC_ACQUIRE)) {
        return ESP_OK;
    }
    ESP_RETURN_ON_FALSE(logging_is_initialised(), ESP_ERR_INVALID_STATE, TAG, "logging_start before logging_init");
    s_ring_buf = heap_caps_malloc(RING_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(s_ring_buf, ESP_ERR_NO_MEM, TAG, "no memory for the %u-byte ring", (unsigned) RING_SIZE);
    logging_ring_init(&s_ring, s_ring_buf, RING_SIZE);

    esp_err_t err = logging_drain_start(&s_ring, &s_drain);

    if (err != ESP_OK) {
        heap_caps_free(s_ring_buf);
        s_ring_buf = NULL;
        ESP_LOGE(TAG, "drain task: %s", esp_err_to_name(err));
        return err;
    }
    __atomic_store_n(&s_ready, true, __ATOMIC_RELEASE);
    return ESP_OK;
}

uint32_t logging_capture_live_dropped(void)
{
    return __atomic_load_n(&s_live_dropped, __ATOMIC_RELAXED);
}

void logging_get_status(logging_status_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!__atomic_load_n(&s_ready, __ATOMIC_ACQUIRE)) {
        return;
    }
    logging_drain_status(out);
    out->sent[LOGGING_CH_UART] = __atomic_load_n(&s_uart_sent, __ATOMIC_RELAXED);
    for (int ch = 0; ch < LOGGING_CH_COUNT; ch++) {
        out->dropped[ch] += __atomic_load_n(&s_ring_dropped[ch], __ATOMIC_RELAXED);
    }
    out->live_dropped += __atomic_load_n(&s_live_dropped, __ATOMIC_RELAXED);
    logging_viewers_status(out);
}
