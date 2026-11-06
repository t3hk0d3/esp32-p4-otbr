// C6 transport of the mux: the on-board ESP32-C6 RCP over SPI2 with OpenThread's SPI framing (spi_frame.hpp, as
// ESP-IDF's esp_spi_spinel_interface.cpp uses it), its INT pin and its reset GPIO. The C6 task (NET_CORE) runs the
// transactions the RCP asks for (INT falling edge); senders run theirs in their own task. Both under the port's
// tx_lock: one transaction at a time. Every transaction is full duplex into a pool slot (R12: the spinel frame at
// slot + MUX_SLOT_HDR, DMA'd in place), so a frame the RCP sends during one of ours is posted like any other. Only the
// owner task opens, closes and resets.
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "board.h"
#include "board_cores.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "radio_mux_priv.h"
#include "sdkconfig.h"

static const char *TAG = "radio_mux";

#define C6_SPI_HOST SPI2_HOST
#define C6_TASK_NAME "radio_mux_c6"
#define C6_TASK_STACK 4096
#define C6_TASK_PRIORITY (CONFIG_OPENTHREAD_TASK_PRIORITY + 1) // above the OpenThread task it feeds
// Minimum time between the end of one transaction and the start of the next. The C6's SPI slave (IDF
// esp_openthread_spi_slave.c) raises INT in its post-transaction ISR, runs the completion callback and only then queues
// its next transaction: clocking before that reads an unloaded slave. On the 160 MHz C6 that ISR path is a few tens
// of microseconds; IDF's host loop (select, eventfd, transaction) leaves tens of microseconds or more. 100 us doubles
// the estimate and costs 6 % of a 1.7 ms receive transaction. Busy-waited (esp_rom_delay_us): a tick is 10 ms.
#define C6_GAP_US 100
#define C6_BAD_HDR_RETRY_MS 15  // a header with a bad pattern: wait, then the same transaction once more (as IDF)
#define C6_TX_TRIES 4           // a frame the RCP did not take (busy): sent again after a tick, this many times in all
#define C6_RESET_PULSE_MS 20    // as rcp_sync's pulse_reset
#define C6_RESET_WAIT_MS 2000   // RESET is posted when the RCP answers after a reset pulse, at the latest after this
#define C6_CS_PRETRANS 2        // as br_core's OpenThread SPI device
#define C6_INPUT_DELAY_NS 100
#define C6_READY_WAIT_MS 1000   // c6_transport_start waits this long for the task's GPIO ISR service
#define BITS_PER_BYTE 8
#define BIT_INT (1u << 0)  // the RCP pulled INT low: it asks for a transaction
#define BIT_WAKE (1u << 1) // a reset started: wait for its deadline

// At least `ms`: pdMS_TO_TICKS rounds down (15 ms is one tick at 100 Hz).
#define TICKS_AT_LEAST(ms) ((TickType_t)(((ms) + portTICK_PERIOD_MS - 1) / portTICK_PERIOD_MS))

_Static_assert(MUX_SLOT_HDR == RADIO_MUX_SPI_HDR_LEN, "a slot's header room holds the SPI header");
_Static_assert(RADIO_MUX_SPI_XFER_ALIGN == MUX_CACHE_LINE, "transactions are clocked in whole cache lines");
_Static_assert(MUX_ALIGN_UP(RADIO_MUX_SPI_HDR_LEN + RADIO_MUX_FRAME_MAX, RADIO_MUX_SPI_XFER_ALIGN) <= MUX_SLOT_SIZE,
               "a receive transaction fits one slot");

static TaskHandle_t s_task;
static SemaphoreHandle_t s_ready; // c6_transport_start only
static bool s_isr_service;        // the C6 task installed the GPIO ISR service on NET_CORE (or it was there)
static atomic_bool s_open;

// Under g_mux.c6.tx_lock: written by the owner (open, close, reset), read by the C6 task and the senders.
static spi_device_handle_t s_dev;
static bool s_bus_up, s_int_ours, s_isr_up;
static bool s_reset_bit;  // our next transaction carries the reset bit (after open and reset) until a valid header
static bool s_reset_wait; // RESET goes to the consumer on the RCP's first header with its reset bit
static int64_t s_reset_deadline_us;
static int64_t s_last_end_us = -1; // when the previous transaction ended (esp_timer); -1: none yet
static uint8_t *s_tx; // DMA: our header, then our frame

// Owner: the mux drives UPDATE and the reset GPIO (since its last open or hold). Cleared when it releases them:
// rcp_sync and c6_hosted reconfigure both pins for their jobs, so every open and hold configures them again.
static bool s_pins_ours;

/* ---- interrupt ---- */

static void IRAM_ATTR int_isr(void *arg)
{
    BaseType_t woken = pdFALSE;

    xTaskNotifyFromISR(s_task, BIT_INT, eSetBits, &woken);
    portYIELD_FROM_ISR(woken);
}

/* ---- transactions (tx_lock held, open) ---- */

// One full-duplex transaction: our header (and the frame already in s_tx after it) out, the RCP's into a slot (the
// discard buffer when the pool is empty: the RCP is never stalled, its frame counts as a drop). A bad header is tried
// once more after C6_BAD_HDR_RETRY_MS. ESP_ERR_INVALID_RESPONSE when it stays bad. Never clocks within C6_GAP_US of
// the previous transaction's end, whoever ran it (the C6 task or a sender), so the slave has re-armed.
static esp_err_t xfer(uint16_t tx_len, uint16_t accept, bool *tx_taken)
{
    mux_port_t *p = &g_mux.c6;
    const radio_mux_spi_hdr_t h = {.reset = s_reset_bit, .accept_len = accept, .data_len = tx_len};
    const size_t len = radio_mux_spi_xfer_len(tx_len, accept);
    uint8_t *slot = mux_slot_take(p);
    uint8_t *buf = slot ? slot : p->discard;
    spi_transaction_t t = {
        .length = len * BITS_PER_BYTE, .rxlength = len * BITS_PER_BYTE, .tx_buffer = s_tx, .rx_buffer = buf,
    };
    radio_mux_spi_result_t r = {0};
    esp_err_t err;

    *tx_taken = false;
    radio_mux_spi_hdr_encode(&h, s_tx);
    for (int attempt = 0;; attempt++) {
        const uint32_t wait_us = radio_mux_spi_gap_wait_us(esp_timer_get_time(), s_last_end_us, C6_GAP_US);

        if (wait_us > 0) {
            esp_rom_delay_us(wait_us);
        }
        err = spi_device_transmit(s_dev, &t);
        s_last_end_us = esp_timer_get_time();
        if (err != ESP_OK) {
            break;
        }
        radio_mux_spi_check(buf, tx_len, accept, RADIO_MUX_FRAME_MAX, &r);
        if (r.valid || attempt > 0) {
            break;
        }
        vTaskDelay(TICKS_AT_LEAST(C6_BAD_HDR_RETRY_MS));
    }
    if (err != ESP_OK || !r.valid) {
        mux_slot_put(p, slot);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "c6: SPI transaction: %s", esp_err_to_name(err));
        } else if (!s_reset_bit) { // a C6 still starting after open or reset is expected to answer garbage
            mux_count_rx_errors(p, 1);
        }
        return err != ESP_OK ? err : ESP_ERR_INVALID_RESPONSE;
    }
    s_reset_bit = false;
    *tx_taken = r.tx_taken;
    xSemaphoreTake(p->rx_lock, portMAX_DELAY);
    if (r.rcp_reset && s_reset_wait) {
        s_reset_wait = false;
        mux_rx_reset(p); // before the RCP's first frames after the reset
    } else if (r.rcp_reset) {
        ESP_LOGI(TAG, "c6: the RCP started");
    }
    if (r.rx_len > 0) {
        mux_rx_frame(p, slot, r.rx_len);
    } else {
        mux_slot_put(p, slot);
    }
    xSemaphoreGive(p->rx_lock);
    return ESP_OK;
}

// The C6 task: one receive transaction per falling edge of INT, as IDF's interface does. The INT level is never read
// to go on: right after a transaction it can still read low before the slave's done callback raised it. The slave
// holds one transaction at a time and lowers INT again (a new edge, a new notify) when it has the next one ready, so
// edges merged into one notify lose nothing.
static void rx_once(void)
{
    bool taken;

    xfer(0, RADIO_MUX_FRAME_MAX, &taken);
}

static TickType_t reset_wait_ticks(void)
{
    TickType_t wait = portMAX_DELAY;

    xSemaphoreTake(g_mux.c6.tx_lock, portMAX_DELAY);
    if (s_reset_wait) {
        const int64_t left_ms = (s_reset_deadline_us - esp_timer_get_time()) / 1000;

        wait = left_ms > 0 ? TICKS_AT_LEAST(left_ms) : 0;
    }
    xSemaphoreGive(g_mux.c6.tx_lock);
    return wait;
}

static void c6_task(void *arg)
{
    mux_port_t *p = &g_mux.c6;
    // Installed here so the GPIO interrupt (INT) lands on NET_CORE; never uninstalled. Already installed is fine.
    const esp_err_t err = gpio_install_isr_service(0);

    s_isr_service = err == ESP_OK || err == ESP_ERR_INVALID_STATE;
    if (!s_isr_service) {
        ESP_LOGE(TAG, "c6: GPIO ISR service: %s", esp_err_to_name(err));
    }
    xSemaphoreGive(s_ready);
    for (;;) {
        uint32_t bits = 0;

        xTaskNotifyWait(0, UINT32_MAX, &bits, reset_wait_ticks());
        xSemaphoreTake(p->tx_lock, portMAX_DELAY);
        if (atomic_load(&s_open) && (bits & BIT_INT)) {
            rx_once();
        }
        if (atomic_load(&s_open) && s_reset_wait && esp_timer_get_time() >= s_reset_deadline_us) {
            s_reset_wait = false;
            ESP_LOGW(TAG, "c6: no answer %d ms after the reset: RESET posted anyway", C6_RESET_WAIT_MS);
            xSemaphoreTake(p->rx_lock, portMAX_DELAY);
            mux_rx_reset(p);
            xSemaphoreGive(p->rx_lock);
        }
        xSemaphoreGive(p->tx_lock);
    }
}

esp_err_t c6_transport_tx(const uint8_t *frame, size_t n)
{
    if (!atomic_load(&s_open) || !s_dev) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!frame || n == 0 || n > RADIO_MUX_FRAME_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    const uint16_t len = (uint16_t)n;
    const uint16_t accept = len < RADIO_MUX_SPI_SMALL_ACCEPT ? RADIO_MUX_SPI_SMALL_ACCEPT : len;
    esp_err_t err = ESP_ERR_TIMEOUT;

    memcpy(s_tx + RADIO_MUX_SPI_HDR_LEN, frame, n);
    for (int i = 0; i < C6_TX_TRIES; i++) {
        bool taken = false;

        err = xfer(len, accept, &taken);
        if (err != ESP_OK) {
            return err; // the driver failed, or the header stayed bad after its retry
        }
        if (taken) {
            return ESP_OK;
        }
        err = ESP_ERR_TIMEOUT;
        if (i + 1 < C6_TX_TRIES) {
            vTaskDelay(1); // the RCP is still handling our previous frame (accept 0)
        }
    }
    ESP_LOGW(TAG, "c6: the RCP did not take a %u-byte frame", (unsigned)n);
    return err;
}

/* ---- open, close, reset (owner) ---- */

// UPDATE high (the C6 boots its boot slot, never its updater: rcp_sync drives it low for its own jobs) and the reset
// GPIO at `run` (high: the C6 runs). Levels are latched before the pins become outputs; configured on every call.
static void pins_set(bool run)
{
    const gpio_config_t out = {
        .pin_bit_mask = (1ULL << P4_PIN_C6_UPDATE) | (1ULL << P4_PIN_C6_RESET),
        .mode = GPIO_MODE_OUTPUT,
    };

    gpio_set_level(P4_PIN_C6_UPDATE, 1);
    gpio_set_level(P4_PIN_C6_RESET, run ? 1 : 0);
    if (gpio_config(&out) == ESP_OK) {
        s_pins_ours = true;
    }
}

// The port's frame pool, the discard buffer and our TX buffer: DMA-capable, cache-line aligned, allocated on the first
// open and kept (a consumer may hold slots past its detach).
static esp_err_t alloc_bufs(mux_port_t *p)
{
    if (p->pool.slots) {
        return ESP_OK;
    }
    const uint32_t caps = MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL;
    uint8_t *slots = heap_caps_aligned_alloc(MUX_CACHE_LINE, (size_t)MUX_SLOT_SIZE * RADIO_MUX_POOL_SLOTS, caps);
    uint8_t *discard = heap_caps_aligned_alloc(MUX_CACHE_LINE, MUX_SLOT_SIZE, caps);
    uint8_t *tx = heap_caps_aligned_alloc(MUX_CACHE_LINE, MUX_SLOT_SIZE, caps);

    if (!slots || !discard || !tx) {
        heap_caps_free(slots);
        heap_caps_free(discard);
        heap_caps_free(tx);
        return ESP_ERR_NO_MEM;
    }
    memset(tx, 0, MUX_SLOT_SIZE);
    p->discard = discard;
    s_tx = tx;
    taskENTER_CRITICAL(&p->lock);
    radio_mux_pool_init(&p->pool, slots, MUX_SLOT_SIZE, RADIO_MUX_POOL_SLOTS);
    taskEXIT_CRITICAL(&p->lock);
    return ESP_OK;
}

// tx_lock held. Frees whatever the open set up; nothing when nothing is (OpenThread's own SPI path may hold the pins).
static void teardown(void)
{
    atomic_store(&s_open, false);
    s_reset_bit = false;
    s_reset_wait = false;
    if (s_isr_up) {
        gpio_isr_handler_remove(P4_PIN_RCP_INT);
        s_isr_up = false;
    }
    if (s_dev) {
        spi_bus_remove_device(s_dev);
        s_dev = NULL;
    }
    if (s_bus_up) {
        spi_bus_free(C6_SPI_HOST);
        s_bus_up = false;
    }
    if (s_int_ours) {
        gpio_reset_pin(P4_PIN_RCP_INT); // the next owner (rcp_sync) sets it up itself
        s_int_ours = false;
    }
}

// tx_lock held.
static esp_err_t bring_up(void)
{
    const gpio_config_t in = {
        .pin_bit_mask = 1ULL << P4_PIN_RCP_INT,
                             .mode = GPIO_MODE_INPUT,
                             .pull_up_en = GPIO_PULLUP_ENABLE,
                             .intr_type = GPIO_INTR_NEGEDGE,
    };
    const spi_bus_config_t bus = {
        .mosi_io_num = P4_PIN_RCP_MOSI,
        .miso_io_num = P4_PIN_RCP_MISO,
        .sclk_io_num = P4_PIN_RCP_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = MUX_SLOT_SIZE,
        .isr_cpu_id = NET_CORE == 0 ? ESP_INTR_CPU_AFFINITY_0 : ESP_INTR_CPU_AFFINITY_1,
    };
    const spi_device_interface_config_t dev = {
        .mode = 0,
        .clock_speed_hz = P4_RCP_SPI_HZ,
        .spics_io_num = P4_PIN_RCP_CS,
        .queue_size = 1,
        .cs_ena_pretrans = C6_CS_PRETRANS,
        .input_delay_ns = C6_INPUT_DELAY_NS,
    };
    spi_device_handle_t handle = NULL;

    ESP_RETURN_ON_FALSE(s_isr_service, ESP_ERR_INVALID_STATE, TAG, "no GPIO ISR service");
    ESP_RETURN_ON_ERROR(gpio_config(&in), TAG, "INT pin");
    s_int_ours = true;
    ESP_RETURN_ON_ERROR(spi_bus_initialize(C6_SPI_HOST, &bus, SPI_DMA_CH_AUTO), TAG, "SPI2 bus");
    s_bus_up = true;
    ESP_RETURN_ON_ERROR(spi_bus_add_device(C6_SPI_HOST, &dev, &handle), TAG, "SPI2 device");
    s_dev = handle;
    ESP_RETURN_ON_ERROR(gpio_isr_handler_add(P4_PIN_RCP_INT, int_isr, NULL), TAG, "INT interrupt");
    s_isr_up = true;
    return ESP_OK;
}

esp_err_t c6_transport_open(char *err, size_t cap)
{
    mux_port_t *p = &g_mux.c6;
    esp_err_t ret;

    err[0] = '\0';
    if (atomic_load(&s_open)) {
        return ESP_OK;
    }
    ret = alloc_bufs(p);
    if (ret != ESP_OK) {
        snprintf(err, cap, "no memory for the C6 buffers");
        return ret;
    }
    xSemaphoreTake(p->tx_lock, portMAX_DELAY);
    ret = bring_up();
    if (ret == ESP_OK) {
        s_reset_bit = true; // the first transaction after open carries it
        s_reset_wait = false;
        atomic_store(&s_open, true);
    } else {
        teardown();
    }
    xSemaphoreGive(p->tx_lock);
    if (ret != ESP_OK) {
        snprintf(err, cap, "C6 SPI open failed: %s", esp_err_to_name(ret));
        return ret;
    }
    // Out of reset only now that INT is armed: a C6 held in reset (mode off) boots and its first request is seen.
    pins_set(true);
    xTaskNotify(s_task, BIT_INT, eSetBits); // the first transaction, and a frame the RCP may already hold
    ESP_LOGI(TAG, "c6: SPI2 open (%d Hz)", P4_RCP_SPI_HZ);
    return ESP_OK;
}

static void shut(bool hold_reset)
{
    mux_port_t *p = &g_mux.c6;
    const bool was_open = atomic_load(&s_open);

    xSemaphoreTake(p->tx_lock, portMAX_DELAY); // a transaction in progress ends first
    teardown();
    xSemaphoreGive(p->tx_lock);
    if (hold_reset) {
        pins_set(false);
    } else if (s_pins_ours) {
        pins_set(true); // the C6 left running, then the pins are someone else's (a job, OpenThread's own path)
        s_pins_ours = false;
    }
    if (was_open) {
        ESP_LOGI(TAG, "c6: SPI2 closed%s", hold_reset ? ", C6 held in reset" : "");
    }
}

void c6_transport_close(void)
{
    shut(true);
}

void c6_transport_release(void)
{
    shut(false);
}

bool c6_transport_is_open(void)
{
    return atomic_load(&s_open);
}

esp_err_t c6_transport_reset(void)
{
    mux_port_t *p = &g_mux.c6;

    xSemaphoreTake(p->tx_lock, portMAX_DELAY); // no transaction while the C6 is down
    if (!atomic_load(&s_open)) {
        xSemaphoreGive(p->tx_lock);
        return ESP_ERR_INVALID_STATE;
    }
    gpio_set_level(P4_PIN_C6_RESET, 0);
    vTaskDelay(TICKS_AT_LEAST(C6_RESET_PULSE_MS));
    gpio_set_level(P4_PIN_C6_RESET, 1);
    s_reset_bit = true;
    s_reset_wait = true;
    s_reset_deadline_us = esp_timer_get_time() + (int64_t)C6_RESET_WAIT_MS * 1000;
    xSemaphoreGive(p->tx_lock);
    xTaskNotify(s_task, BIT_WAKE, eSetBits);
    return ESP_OK;
}

esp_err_t c6_transport_start(void)
{
    s_ready = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_ready, ESP_ERR_NO_MEM, TAG, "c6 ready semaphore");
    if (xTaskCreatePinnedToCore(c6_task, C6_TASK_NAME, C6_TASK_STACK, NULL, C6_TASK_PRIORITY, &s_task, NET_CORE) !=
            pdPASS) {
        vSemaphoreDelete(s_ready);
        s_ready = NULL;
        return ESP_ERR_NO_MEM;
    }
    if (xSemaphoreTake(s_ready, pdMS_TO_TICKS(C6_READY_WAIT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "c6: the C6 task did not start in %d ms", C6_READY_WAIT_MS); // the semaphore stays
        return ESP_OK;
    }
    vSemaphoreDelete(s_ready);
    s_ready = NULL;
    return ESP_OK;
}
