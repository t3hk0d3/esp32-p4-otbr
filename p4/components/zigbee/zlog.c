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

#include "board_cores.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "logging.h"
#include "zigbee_priv.h"
#include "zigbee_zlog_map.h"
#include "zigbee_zlog_ring.h"

// The patch's exports and import (declared in no generated header).
void ziggurat_log_set_level(uint8_t level);

static const char *TAG = "ziggurat";

#define ZLOG_RING_SIZE 16384 // a power of two
#define ZLOG_DRAIN_MS 20
#define ZLOG_DRAIN_STACK 4096
#define ZLOG_DRAIN_PRIORITY 2

// The ring's memory and the drain's line buffer are allocated by zigbee_zlog_start_drain (the first bind), so a board
// that never binds ziggurat spends only these few words. Until then ziggurat_platform_log drops and counts.
static zigbee_zlog_ring_t s_ring;
static char *s_line; // ZIGBEE_ZLOG_LINE_MAX + 1
static atomic_uchar s_level = ATOMIC_VAR_INIT(3); // the mapped level (info until the listener's first call)

// Any context, ziggurat's critical sections included: copies and returns.
void ziggurat_platform_log(uint8_t level, const uint8_t *msg, size_t len)
{
    zigbee_zlog_ring_put(&s_ring, level, msg, len);
}

static void drain_task(void *arg)
{
    uint32_t reported = 0;

    for (;;) {
        uint8_t level;
        size_t len;

        while (zigbee_zlog_ring_get(&s_ring, &level, s_line, &len)) {
            ESP_LOG_LEVEL(zigbee_zlog_to_esp(level), TAG, "%s", s_line);
        }
        const uint32_t dropped = zigbee_zlog_ring_dropped(&s_ring);

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
        // PSRAM first: the image runs from PSRAM (XIP) and ziggurat's own heap is there, so the producer's copy in
        // ziggurat's critical sections touches the same memory ziggurat already uses with interrupts masked. Internal RAM
        // only when PSRAM is full.
        uint8_t *ring = heap_caps_malloc(ZLOG_RING_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

        if (s_line == NULL) {
            s_line = heap_caps_malloc(ZIGBEE_ZLOG_LINE_MAX + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        }
        if (ring == NULL) {
            ring = heap_caps_malloc(ZLOG_RING_SIZE, MALLOC_CAP_8BIT);
        }
        if (s_line == NULL) {
            s_line = heap_caps_malloc(ZIGBEE_ZLOG_LINE_MAX + 1, MALLOC_CAP_8BIT);
        }
        if (ring == NULL || s_line == NULL) {
            heap_caps_free(ring);
            return ESP_ERR_NO_MEM;
        }
        zigbee_zlog_ring_init(&s_ring, ring, ZLOG_RING_SIZE); // before the producer (the zigbee task) exists
        if (xTaskCreatePinnedToCore(drain_task, "zlog", ZLOG_DRAIN_STACK, NULL, ZLOG_DRAIN_PRIORITY, &task, MGMT_CORE) !=
                pdPASS) {
            zigbee_zlog_ring_init(&s_ring, NULL, 0); // a retry allocates again
            heap_caps_free(ring);
            ESP_LOGE(TAG, "zlog task: no memory");
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

void zigbee_zlog_apply(void)
{
    ziggurat_log_set_level(atomic_load(&s_level));
}
