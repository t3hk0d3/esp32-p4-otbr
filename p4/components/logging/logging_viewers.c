// Live viewers of the logging component: up to LOGGING_VIEWERS_MAX readers (the live log WebSocket's clients), each
// with its own level and an 8 KB queue in PSRAM. The drain task copies each line into the queue of every viewer whose
// level it passes and calls that viewer's wake; a full queue drops the line for that viewer only and counts it. The
// reader takes lines off its queue on its own task. The highest open level is a channel of its own for the generation
// gate (logging_viewers_max_changed): with no viewer it is off, so lines nobody else wants are never formatted.
//
// Locks: s_lock guards the slots (open, level) and is held by the drain while it copies a line (no blocking inside);
// s_gate_lock keeps gate updates in order, so the last one carries the levels in force. Nothing here logs.

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "logging_priv.h"
#include "logging_ring.h"

struct logging_viewer {
    bool open;             /**< Under s_lock. */
    esp_log_level_t level; /**< Under s_lock. */
    void (*wake)(void *ctx);
    void *ctx;
    uint8_t *mem;          /**< PSRAM: the queue, then the reader's line buffer. */
    logging_ring_t ring;   /**< The queue: the drain produces, the reader consumes. */
    uint32_t reported;     /**< Reader only: ring drops already reported. */
};

#define VIEWER_MEM (LOGGING_VIEWER_QUEUE + LOGGING_LINE_MAX)

enum { LOCKS_NONE, LOCKS_MAKING, LOCKS_READY };

static logging_viewer_t s_viewers[LOGGING_VIEWERS_MAX];
static StaticSemaphore_t s_lock_buf, s_gate_lock_buf;
static SemaphoreHandle_t s_lock, s_gate_lock;
static uint32_t s_locks; // atomic: LOCKS_*
static uint32_t s_live_sent;    // atomic
static uint32_t s_live_dropped; // atomic

/** The two mutexes, made once by the first caller; static, so this cannot fail. */
static void ensure_locks(void)
{
    uint32_t expected = LOCKS_NONE;

    if (__atomic_load_n(&s_locks, __ATOMIC_ACQUIRE) == LOCKS_READY) {
        return;
    }
    if (__atomic_compare_exchange_n(&s_locks, &expected, LOCKS_MAKING, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
        s_gate_lock = xSemaphoreCreateMutexStatic(&s_gate_lock_buf);
        __atomic_store_n(&s_locks, LOCKS_READY, __ATOMIC_RELEASE);
        return;
    }
    while (__atomic_load_n(&s_locks, __ATOMIC_ACQUIRE) != LOCKS_READY) {
        vTaskDelay(1);
    }
}

static bool level_valid(esp_log_level_t level)
{
    return level >= ESP_LOG_NONE && level <= ESP_LOG_VERBOSE;
}

/** Hand the highest open level to the settings code, which recomputes and publishes every gate. */
static void gate_update(void)
{
    esp_log_level_t max = ESP_LOG_NONE;

    xSemaphoreTake(s_gate_lock, portMAX_DELAY);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < LOGGING_VIEWERS_MAX; i++) {
        if (s_viewers[i].open && s_viewers[i].level > max) {
            max = s_viewers[i].level;
        }
    }
    xSemaphoreGive(s_lock);
    logging_viewers_max_changed(max);
    xSemaphoreGive(s_gate_lock);
}

logging_viewer_t *logging_viewer_open(esp_log_level_t level, void (*wake)(void *ctx), void *ctx)
{
    logging_viewer_t *v = NULL;
    uint8_t *mem;

    if (!level_valid(level)) {
        return NULL;
    }
    ensure_locks();
    mem = heap_caps_malloc(VIEWER_MEM, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!mem) {
        return NULL;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < LOGGING_VIEWERS_MAX && !v; i++) {
        if (!s_viewers[i].open) {
            v = &s_viewers[i];
        }
    }
    if (v) {
        v->level = level;
        v->wake = wake;
        v->ctx = ctx;
        v->mem = mem;
        v->reported = 0;
        logging_ring_init(&v->ring, mem, LOGGING_VIEWER_QUEUE);
        v->open = true;
    }
    xSemaphoreGive(s_lock);
    if (!v) {
        heap_caps_free(mem);
        return NULL;
    }
    gate_update();
    return v;
}

void logging_viewer_set_level(logging_viewer_t *viewer, esp_log_level_t level)
{
    if (!viewer || !level_valid(level)) {
        return;
    }
    ensure_locks();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    viewer->level = level;
    xSemaphoreGive(s_lock);
    gate_update();
}

bool logging_viewer_read(logging_viewer_t *viewer, logging_line_t *out, uint32_t *dropped_since)
{
    if (dropped_since) {
        *dropped_since = 0;
    }
    if (!viewer || !viewer->mem) {
        return false;
    }
    if (dropped_since) {
        uint32_t dropped = logging_ring_dropped(&viewer->ring);

        *dropped_since = dropped - viewer->reported;
        viewer->reported = dropped;
    }
    return logging_ring_pop(&viewer->ring, out, (char *) viewer->mem + LOGGING_VIEWER_QUEUE, LOGGING_LINE_MAX);
}

void logging_viewer_close(logging_viewer_t *viewer)
{
    uint8_t *mem;

    if (!viewer) {
        return;
    }
    ensure_locks();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!viewer->open) {
        xSemaphoreGive(s_lock);
        return;
    }
    viewer->open = false;
    mem = viewer->mem;
    viewer->mem = NULL;
    xSemaphoreGive(s_lock);
    heap_caps_free(mem); // the drain touches a queue only under s_lock and only while it is open
    gate_update();
}

void logging_viewers_fanout(const logging_line_t *line)
{
    size_t len = line->len < LOGGING_LINE_MAX ? line->len : LOGGING_LINE_MAX;
    bool queued = false;
    bool dropped = false;

    if (__atomic_load_n(&s_locks, __ATOMIC_ACQUIRE) != LOCKS_READY) {
        return; // no viewer was ever opened
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < LOGGING_VIEWERS_MAX; i++) {
        logging_viewer_t *v = &s_viewers[i];
        logging_ring_slot_t slot;

        if (!v->open || line->level > v->level) {
            continue;
        }
        if (!logging_ring_reserve(&v->ring, len, &slot)) {
            dropped = true; // counted in the ring: the reader reports it
            continue;
        }
        memcpy(slot.text, line->text, len);
        logging_ring_commit(&v->ring, &slot, line, len);
        queued = true;
        if (v->wake) {
            v->wake(v->ctx);
        }
    }
    xSemaphoreGive(s_lock);
    if (queued) {
        __atomic_fetch_add(&s_live_sent, 1, __ATOMIC_RELAXED);
    }
    if (dropped) {
        __atomic_fetch_add(&s_live_dropped, 1, __ATOMIC_RELAXED);
    }
}

void logging_viewers_status(logging_status_t *out)
{
    uint8_t n = 0;

    if (__atomic_load_n(&s_locks, __ATOMIC_ACQUIRE) == LOCKS_READY) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        for (int i = 0; i < LOGGING_VIEWERS_MAX; i++) {
            n += s_viewers[i].open ? 1 : 0;
        }
        xSemaphoreGive(s_lock);
    }
    out->viewers = n;
    out->live_sent += __atomic_load_n(&s_live_sent, __ATOMIC_RELAXED);
    out->live_dropped += __atomic_load_n(&s_live_dropped, __ATOMIC_RELAXED);
}

void logging_viewers_reset(void)
{
    if (__atomic_load_n(&s_locks, __ATOMIC_ACQUIRE) == LOCKS_READY) {
        for (int i = 0; i < LOGGING_VIEWERS_MAX; i++) {
            logging_viewer_close(&s_viewers[i]);
        }
    }
    __atomic_store_n(&s_live_sent, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&s_live_dropped, 0, __ATOMIC_RELAXED);
}
