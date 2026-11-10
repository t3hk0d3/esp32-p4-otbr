// ziggurat's tracing events (the log patch, crates/ziggurat-ot/src/log.rs) in the board log, tag "ziggurat".
//
// ziggurat may log inside its critical section (interrupts off on that core), where ESP_LOG's locks are not allowed:
// ziggurat_platform_log only copies the line into a lock-free single-producer ring (every ziggurat_* call runs on the
// zigbee task), and a low-priority task prints it with ESP_LOG_LEVEL, so the lines go through the logging capture like
// any other. Lines that do not fit are dropped and counted.
//
// The level follows the logging component's level of the tag "ziggurat": the listener stores it and asks the zigbee
// task to call ziggurat_log_set_level (flag ZIGBEE_FLAG_LOGLEVEL); the zigbee task also applies it right after
// ziggurat_start.
#include <inttypes.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#include "board_cores.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "logging.h"
#include "zigbee_priv.h"
#include "zigbee_zlog_map.h"

// The patch's exports and import (declared in no generated header).
void ziggurat_log_set_level(uint8_t level);

static const char *TAG = "ziggurat";

#define ZLOG_RING_SIZE 16384 // a power of two
#define ZLOG_LINE_MAX 512
#define ZLOG_DRAIN_MS 20
#define ZLOG_DRAIN_STACK 4096
#define ZLOG_DRAIN_PRIORITY 2

static uint8_t s_ring[ZLOG_RING_SIZE];
static atomic_uint s_head; // written by the producer only
static atomic_uint s_tail; // written by the drain task only
static atomic_uint s_dropped;
static atomic_uchar s_level = ATOMIC_VAR_INIT(3); // the mapped level (info until the listener's first call)

static void ring_put(uint32_t at, const void *src, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        s_ring[(at + i) & (ZLOG_RING_SIZE - 1)] = ((const uint8_t *)src)[i];
    }
}

static void ring_get(uint32_t at, void *dst, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        ((uint8_t *)dst)[i] = s_ring[(at + i) & (ZLOG_RING_SIZE - 1)];
    }
}

// Any context, ziggurat's critical sections included: copies and returns.
void ziggurat_platform_log(uint8_t level, const uint8_t *msg, size_t len)
{
    if (len > ZLOG_LINE_MAX) {
        len = ZLOG_LINE_MAX;
    }
    const uint32_t head = atomic_load_explicit(&s_head, memory_order_relaxed);
    const uint32_t tail = atomic_load_explicit(&s_tail, memory_order_acquire);
    const size_t need = 3 + len;

    if (ZLOG_RING_SIZE - (head - tail) < need) {
        atomic_fetch_add_explicit(&s_dropped, 1, memory_order_relaxed);
        return;
    }
    const uint8_t hdr[3] = {level, (uint8_t)(len & 0xff), (uint8_t)(len >> 8)};

    ring_put(head, hdr, 3);
    ring_put(head + 3, msg, len);
    atomic_store_explicit(&s_head, head + need, memory_order_release);
}

static void drain_task(void *arg)
{
    static char line[ZLOG_LINE_MAX + 1];
    uint32_t reported = 0;

    for (;;) {
        uint32_t tail = atomic_load_explicit(&s_tail, memory_order_relaxed);
        const uint32_t head = atomic_load_explicit(&s_head, memory_order_acquire);

        while (tail != head) {
            uint8_t hdr[3];

            ring_get(tail, hdr, 3);
            const size_t len = hdr[1] | (hdr[2] << 8);

            ring_get(tail + 3, line, len);
            line[len] = '\0';
            tail += 3 + len;
            atomic_store_explicit(&s_tail, tail, memory_order_release);
            ESP_LOG_LEVEL(zigbee_zlog_to_esp(hdr[0]), TAG, "%s", line);
        }
        const uint32_t dropped = atomic_load_explicit(&s_dropped, memory_order_relaxed);

        if (dropped != reported) {
            ESP_LOGW(TAG, "%" PRIu32 " lines dropped (ring full)", dropped - reported);
            reported = dropped;
        }
        vTaskDelay(pdMS_TO_TICKS(ZLOG_DRAIN_MS));
    }
}

// Runs on the task that changed the level (and once, in zigbee_zlog_init): stores, and asks the zigbee task when it
// exists. Never blocks, never calls logging_*.
static void level_cb(esp_log_level_t effective)
{
    atomic_store(&s_level, zigbee_zlog_from_esp(effective));
    if (atomic_load(&g_zigbee.up)) {
        zigbee_post_flags(ZIGBEE_FLAG_LOGLEVEL);
    }
}

esp_err_t zigbee_zlog_init(void)
{
    return logging_level_listener_add(TAG, level_cb);
}

esp_err_t zigbee_zlog_start_drain(void)
{
    static TaskHandle_t task;

    if (task == NULL) {
        ESP_RETURN_ON_FALSE(xTaskCreatePinnedToCore(drain_task, "zlog", ZLOG_DRAIN_STACK, NULL, ZLOG_DRAIN_PRIORITY, &task,
                                                    MGMT_CORE) == pdPASS,
                            ESP_ERR_NO_MEM, TAG, "zlog task");
    }
    return ESP_OK;
}

void zigbee_zlog_apply(void)
{
    ziggurat_log_set_level(atomic_load(&s_level));
}
