// Maintenance jumper, receive side: the C6 ROM banner shows that the wire is there; everything else the C6 prints on
// UART0 goes to a ring buffer (GET /device/radio/log) and, line by line, to the P4 log.
#include <stdatomic.h>
#include "board_cores.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "board.h"
#include "jumper.h"
#include "log_limiter.h"
#include "log_ring.h"
#include "rcp_sync.h"
#include "uart_handoff.h"

static const char *TAG = "rcp_sync";

#define JUMPER_UART_NUM   ((uart_port_t)JUMPER_UART_PORT)
#define UART_RX_BUF       2048
#define READ_CHUNK        256
#define READ_TIMEOUT_MS   100
#define PAUSE_WAIT_MS     1000
#define HANDOFF_POLL_MS   20
#define REINSTALL_WAIT_MS 1000
/* After a UART pause/resume handoff the UART driver (and its ISR) is installed from this task, i.e. on core 1: harmless. */
#define LOG_CORE          MGMT_CORE
#define LOG_TASK_STACK    4096
/* Same as the console REPL task (also core 1, priority 2): the two share the core, so a flood of C6 lines cannot starve the console;
 * below the HTTP server. The forwarding is rate limited (log_limiter.h); the UART driver buffers while this task waits. */
#define LOG_TASK_PRIORITY 2
#define LOG_PREFIX        "c6: "

static struct {
    SemaphoreHandle_t lock;     // guards the ring and the handoff state
    log_ring_t ring;            // bytes, read by GET /device/radio/log
    log_ring_t lines;           // line assembly only (no storage), used by the task alone, so logging happens outside the lock
    log_limiter_t limiter;      // forwarding to the P4 log, used by the task alone
    uart_handoff_t handoff;
    atomic_bool present;        // the ROM banner was seen
    uint8_t *storage;
    bool started;
} s;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void on_line(void *ctx, const char *line)
{
    if (log_ring_is_rom_banner(line)) {
        atomic_store(&s.present, true);
    }
    if (log_limiter_allow(&s.limiter, now_ms())) {
        ESP_LOGI(TAG, LOG_PREFIX "%s", line);
    }
}

// A C6 in a boot loop prints without pause: one summary line per second instead of the lines that were dropped.
static void log_suppressed_summary(void)
{
    const uint32_t n = log_limiter_take_summary(&s.limiter, now_ms());

    if (n > 0) {
        ESP_LOGW(TAG, LOG_PREFIX "%u lines suppressed (still in GET /device/radio/log)", (unsigned)n);
    }
}

static esp_err_t install_uart(void)
{
    const uart_config_t config = {
        .baud_rate = JUMPER_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_driver_install(JUMPER_UART_NUM, UART_RX_BUF, 0, 0, NULL, 0);

    if (err == ESP_OK) {
        err = uart_param_config(JUMPER_UART_NUM, &config);
    }
    if (err == ESP_OK) {
        // Receive pin only; the driver pulls it up, so an unwired RX idles high. JUMPER_TX is never routed here.
        err = uart_set_pin(JUMPER_UART_NUM, UART_PIN_NO_CHANGE, P4_PIN_JUMPER_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }
    if (err != ESP_OK) {
        uart_driver_delete(JUMPER_UART_NUM);
    }
    return err;
}

// One receiver poll; *flasher_owns is true while the UART is not the receiver's (nothing to read).
static handoff_action_t poll_handoff(bool *flasher_owns)
{
    xSemaphoreTake(s.lock, portMAX_DELAY);
    const handoff_action_t action = uart_handoff_poll(&s.handoff);

    *flasher_owns = !uart_handoff_receiver_owns_uart(&s.handoff);
    xSemaphoreGive(s.lock);
    return action;
}

static void log_task(void *arg)
{
    uint8_t chunk[READ_CHUNK];

    for (;;) {
        bool flasher_owns;
        const handoff_action_t action = poll_handoff(&flasher_owns);

        if (action == HANDOFF_ACTION_RELEASE) {
            uart_driver_delete(JUMPER_UART_NUM);
            xSemaphoreTake(s.lock, portMAX_DELAY);
            uart_handoff_released(&s.handoff);
            xSemaphoreGive(s.lock);
            continue;
        }
        if (action == HANDOFF_ACTION_REINSTALL) {
            while (install_uart() != ESP_OK) { // the handoff is RUN again: a new pause request waits for the next poll
                vTaskDelay(pdMS_TO_TICKS(REINSTALL_WAIT_MS));
            }
            continue;
        }
        if (flasher_owns) {
            vTaskDelay(pdMS_TO_TICKS(HANDOFF_POLL_MS));
            continue;
        }
        const int n = uart_read_bytes(JUMPER_UART_NUM, chunk, sizeof(chunk), pdMS_TO_TICKS(READ_TIMEOUT_MS));
        if (n > 0) {
            xSemaphoreTake(s.lock, portMAX_DELAY);
            log_ring_push(&s.ring, chunk, (size_t)n);
            xSemaphoreGive(s.lock);
            log_ring_push(&s.lines, chunk, (size_t)n); // logs the complete lines, outside the lock
        }
        log_suppressed_summary();
    }
}

esp_err_t jumper_start(void)
{
    if (s.started) {
        return ESP_OK;
    }
    // Allocated once, whatever a failed start left behind: a retry reuses them.
    if (!s.storage) {
        s.storage = heap_caps_calloc(1, RCP_SYNC_LOG_CAPACITY, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!s.storage) {
        s.storage = heap_caps_calloc(1, RCP_SYNC_LOG_CAPACITY, MALLOC_CAP_8BIT);
    }
    if (!s.lock) {
        s.lock = xSemaphoreCreateMutex();
    }
    if (!s.storage || !s.lock) {
        return ESP_ERR_NO_MEM;
    }
    log_ring_init(&s.ring, s.storage, RCP_SYNC_LOG_CAPACITY, NULL, NULL);
    log_ring_init(&s.lines, NULL, 0, on_line, NULL);
    log_limiter_init(&s.limiter, LOG_LIMITER_RATE_PER_S);
    uart_handoff_init(&s.handoff);
    esp_err_t err = install_uart();

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "C6 console receiver: %s", esp_err_to_name(err));
        return err;
    }
    if (xTaskCreatePinnedToCore(log_task, "c6_log", LOG_TASK_STACK, NULL, LOG_TASK_PRIORITY, NULL, LOG_CORE) != pdPASS) {
        uart_driver_delete(JUMPER_UART_NUM);
        return ESP_ERR_NO_MEM;
    }
    s.started = true;
    return ESP_OK;
}

esp_err_t jumper_log_pause(void)
{
    if (!s.started) {
        return ESP_OK;
    }
    xSemaphoreTake(s.lock, portMAX_DELAY);
    const bool accepted = uart_handoff_request_pause(&s.handoff);

    xSemaphoreGive(s.lock);
    for (int waited = 0; accepted && waited <= PAUSE_WAIT_MS; waited += HANDOFF_POLL_MS) {
        xSemaphoreTake(s.lock, portMAX_DELAY);
        const bool paused = uart_handoff_is_paused(&s.handoff);

        xSemaphoreGive(s.lock);
        if (paused) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(HANDOFF_POLL_MS));
    }
    return ESP_ERR_TIMEOUT; // the UART is not free; jumper_log_resume cancels the request
}

void jumper_log_resume(void)
{
    if (!s.started) {
        return;
    }
    xSemaphoreTake(s.lock, portMAX_DELAY);
    uart_handoff_request_resume(&s.handoff);
    xSemaphoreGive(s.lock);
}

bool rcp_sync_jumper_present(void)
{
    return atomic_load(&s.present);
}

size_t rcp_sync_log_read(char *buf, size_t cap)
{
    if (!s.started || !buf) {
        return 0;
    }
    xSemaphoreTake(s.lock, portMAX_DELAY);
    const size_t n = log_ring_read(&s.ring, buf, cap);
    xSemaphoreGive(s.lock);
    return n;
}
