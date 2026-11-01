#include <string.h>
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "board.h"
#include "base_image.h"
#include "jumper.h"
#include "link.h"
#include "rcp_sync_proto.h"

static const char *TAG = "rcp_sync";

/* Not SPI2_HOST, which the OpenThread spinel SPI interface owns: after an abandoned OpenThread start (the RCP
 * incompatible fault) that driver stays registered on SPI2 and cannot be released from outside, so a push has
 * to use another host. The bus pins are GPIO-matrix routed; a second claimant only logs "GPIO conflict, overwritten"
 * (esp_driver_spi spi_common.c, s_spi_common_gpio_check_reserve) and takes over the routing. */
#define SPI_HOST            SPI3_HOST
#define DMA_ALIGN           64
#define FRAME_MAX_LEN       (RCP_SYNC_PROTO_HEADER_LEN + RCP_SYNC_PROTO_MAX_PAYLOAD + RCP_SYNC_PROTO_CRC_LEN)
#define BUF_LEN             ((FRAME_MAX_LEN + DMA_ALIGN - 1) & ~(DMA_ALIGN - 1))
#define RESET_PULSE_MS      20
#define UPDATER_BOOT_MS     300   /* bootloader samples GPIO6 ~90 ms after reset, then starts c6_updater */
#define READY_TIMEOUT_MS    1000  /* wait for "ready for request"; covers the updater still booting */
#define INT_HIGH_WAIT_MS    20    /* the C6 raises INT once it has taken the request */
#define POLL_SPIN_US        2000

static bool s_ctl_ready;
static spi_device_handle_t s_dev;
static bool s_bus_up;
static uint8_t *s_tx;
static uint8_t *s_rx;

static esp_err_t ctl_init(void)
{
    if (s_ctl_ready) {
        return ESP_OK;
    }
    const gpio_config_t out = {
        .pin_bit_mask = (1ULL << P4_PIN_C6_UPDATE) | (1ULL << P4_PIN_C6_RESET),
        .mode = GPIO_MODE_OUTPUT,
    };
    // Latch both levels high (boot slot, C6 running) before the pins become outputs.
    gpio_set_level(P4_PIN_C6_UPDATE, 1);
    gpio_set_level(P4_PIN_C6_RESET, 1);
    ESP_RETURN_ON_ERROR(gpio_config(&out), TAG, "gpio config");
    s_ctl_ready = true;
    return ESP_OK;
}

static void pulse_reset(void)
{
    gpio_set_level(P4_PIN_C6_RESET, 0);
    vTaskDelay(pdMS_TO_TICKS(RESET_PULSE_MS));
    gpio_set_level(P4_PIN_C6_RESET, 1);
}

esp_err_t rcp_sync_boot_rcp(void)
{
    ESP_RETURN_ON_ERROR(ctl_init(), TAG, "ctl init");
    jumper_start(); // capture the C6 console from its first boot line on (best effort)
    gpio_set_level(P4_PIN_C6_UPDATE, 1);
    pulse_reset();
    return ESP_OK;
}

static void link_release(void)
{
    if (s_dev) {
        spi_bus_remove_device(s_dev);
        s_dev = NULL;
    }
    if (s_bus_up) {
        spi_bus_free(SPI_HOST);
        s_bus_up = false;
    }
    heap_caps_free(s_tx);
    heap_caps_free(s_rx);
    s_tx = s_rx = NULL;
    if (ctl_init() == ESP_OK) {
        gpio_set_level(P4_PIN_C6_UPDATE, 1);
    }
    // Leave the link pins to the next owner (the OpenThread SPI driver) in their reset state.
    gpio_reset_pin(P4_PIN_RCP_INT);
}

static esp_err_t link_enter_updater(void)
{
    link_release();
    ESP_RETURN_ON_ERROR(ctl_init(), TAG, "ctl init");
    // The jumper receiver listens during the reset below: the C6 ROM banner it sees shows that the wire is fitted.
    if (jumper_start() != ESP_OK) {
        ESP_LOGW(TAG, "jumper receiver not available, detection skipped");
    }
    gpio_set_level(P4_PIN_C6_UPDATE, 0);
    pulse_reset();
    vTaskDelay(pdMS_TO_TICKS(UPDATER_BOOT_MS));

    const gpio_config_t in = {
        .pin_bit_mask = 1ULL << P4_PIN_RCP_INT,
                             .mode = GPIO_MODE_INPUT,
                             .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&in), TAG, "int config");

    const spi_bus_config_t bus = {
        .mosi_io_num = P4_PIN_RCP_MOSI,
        .miso_io_num = P4_PIN_RCP_MISO,
        .sclk_io_num = P4_PIN_RCP_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = BUF_LEN,
    };
    const spi_device_interface_config_t dev = {
        .mode = 0,
        .clock_speed_hz = P4_RCP_SPI_HZ,
        .spics_io_num = P4_PIN_RCP_CS,
        .queue_size = 1,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(SPI_HOST, &bus, SPI_DMA_CH_AUTO), TAG, "bus init");
    s_bus_up = true;
    esp_err_t err = spi_bus_add_device(SPI_HOST, &dev, &s_dev);
    if (err == ESP_OK) {
        s_tx = heap_caps_aligned_calloc(DMA_ALIGN, 1, BUF_LEN, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        s_rx = heap_caps_aligned_calloc(DMA_ALIGN, 1, BUF_LEN, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        err = (s_tx && s_rx) ? ESP_OK : ESP_ERR_NO_MEM;
    }
    if (err != ESP_OK) {
        link_release();
    }
    return err;
}

/* Wait until INT reads `level`. Spins briefly (reply phases are short), then yields. */
static bool wait_int(int level, uint32_t timeout_ms)
{
    const int64_t start = esp_timer_get_time();
    const int64_t limit = (int64_t)timeout_ms * 1000;

    for (;;) {
        if (gpio_get_level(P4_PIN_RCP_INT) == level) {
            return true;
        }
        int64_t waited = esp_timer_get_time() - start;
        if (waited >= limit) {
            return false;
        }
        if (waited < POLL_SPIN_US) {
            esp_rom_delay_us(20);
        } else {
            vTaskDelay(1);
        }
    }
}

static esp_err_t clock_bytes(size_t len)
{
    spi_transaction_t t = {.length = len * 8, .tx_buffer = s_tx, .rx_buffer = s_rx};
    return spi_device_polling_transmit(s_dev, &t);
}

static esp_err_t link_transact(const uint8_t *req, size_t req_len, uint8_t *reply, size_t cap, size_t *reply_len,
                               uint32_t timeout_ms)
{
    if (!s_dev || req_len > FRAME_MAX_LEN || cap > FRAME_MAX_LEN) {
        return ESP_ERR_INVALID_STATE;
    }
    *reply_len = 0;

    // Request phase: C6 pulls INT low when its receive buffer is queued.
    ESP_RETURN_ON_FALSE(wait_int(0, READY_TIMEOUT_MS), ESP_ERR_TIMEOUT, TAG, "C6 not ready for request");
    memset(s_tx, 0, BUF_LEN);
    memcpy(s_tx, req, req_len);
    ESP_RETURN_ON_ERROR(clock_bytes((req_len + 3) & ~3u), TAG, "request clock");

    // Reply phase: INT goes high when the C6 has taken the request, low again when the reply is queued.
    // If the reply is so fast that the high level is missed, carry on; a premature clock yields an
    // undecodable reply, which the caller handles like a NAK.
    if (!wait_int(1, INT_HIGH_WAIT_MS)) {
        ESP_LOGW(TAG, "INT did not go high after the request");
    }
    ESP_RETURN_ON_FALSE(wait_int(0, timeout_ms), ESP_ERR_TIMEOUT, TAG, "no reply");
    memset(s_tx, 0, BUF_LEN);
    size_t n = (cap + 3) & ~3u;
    ESP_RETURN_ON_ERROR(clock_bytes(n), TAG, "reply clock");
    memcpy(reply, s_rx, cap);
    *reply_len = cap;
    return ESP_OK;
}

const rcp_sync_link_t rcp_sync_link_spi = {
    .enter_updater = link_enter_updater,
    .transact = link_transact,
    .release = link_release,
    .jumper_present = rcp_sync_jumper_present,
    .rom_recover = rcp_sync_rom_recover,
    .base_version = rcp_sync_base_image_version,
};
