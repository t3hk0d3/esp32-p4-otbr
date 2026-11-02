// The device link of c6_hosted: SDMMC slot 1 on the C6 lines, the ESP slave link (essl_sdio) on top, and the public API.

#include <string.h>
#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_serial_slave_link/essl.h"
#include "esp_serial_slave_link/essl_sdio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"
#include "board.h"
#include "c6_hosted.h"
#include "hosted_frame.h"
#include "hosted_session.h"

static const char *TAG = "c6_hosted";

#define SDIO_SLOT          SDMMC_HOST_SLOT_1
#define RESET_PULSE_MS     20
#define BOOT_WAIT_MS       500   /* ESP-Hosted starts its SDIO slave well within this after a reset */
#define CARD_INIT_TRIES    5     /* then BOOT_WAIT_MS apart: a slow start still answers */
#define ESSL_TIMEOUT_MS    1000
#define SLAVE_RX_BUF       1536  /* the slave's receive buffer size (ESP_RX_BUFFER_SIZE) */
#define OPEN_DATA_PATH     (1u << 0)
/* At least one tick between looks at the slave (pdMS_TO_TICKS(5) is 0 at 100 Hz): the push must not starve other tasks. */
#define POLL_TICKS         (pdMS_TO_TICKS(5) > 0 ? pdMS_TO_TICKS(5) : 1)
#define DMA_ALIGN          64
#define BLOCK              512   /* reads and writes may round up to a whole block */

static sdmmc_card_t *s_card;
static essl_handle_t s_essl;
static bool s_host_up;
static uint8_t *s_tx;
static uint8_t *s_rx;

static void pulse_reset_normal_boot(void)
{
    const gpio_config_t out = {
        .pin_bit_mask = (1ULL << P4_PIN_C6_UPDATE) | (1ULL << P4_PIN_C6_RESET),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_set_level(P4_PIN_C6_UPDATE, 1); // the boot slot (ESP-Hosted), not the updater
    gpio_set_level(P4_PIN_C6_RESET, 1);
    gpio_config(&out);
    gpio_set_level(P4_PIN_C6_RESET, 0);
    vTaskDelay(pdMS_TO_TICKS(RESET_PULSE_MS));
    gpio_set_level(P4_PIN_C6_RESET, 1);
}

/* sdmmc_card_init allocates a bounce buffer (SDMMC_HOST_FLAG_ALLOC_ALIGNED_BUF) that nothing in IDF frees. */
static void card_clear(void)
{
    if (s_card) {
        heap_caps_free(s_card->host.dma_aligned_buffer);
        memset(s_card, 0, sizeof(*s_card));
    }
}

static void link_release(void)
{
    if (s_essl) {
        essl_sdio_deinit_dev(s_essl);
        s_essl = NULL;
    }
    card_clear();
    free(s_card);
    s_card = NULL;
    if (s_host_up) {
        sdmmc_host_deinit();
        s_host_up = false;
    }
    heap_caps_free(s_tx);
    heap_caps_free(s_rx);
    s_tx = s_rx = NULL;
    // Leave the lines to the SPI link of rcp_sync in their reset state.
    const int pins[] = {P4_PIN_C6_SDIO_CLK, P4_PIN_C6_SDIO_CMD, P4_PIN_C6_SDIO_D0, P4_PIN_C6_SDIO_D1, P4_PIN_C6_SDIO_D2,
                        P4_PIN_C6_SDIO_D3
                       };
    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
        gpio_reset_pin(pins[i]);
    }
}

static esp_err_t link_start(void)
{
    link_release();
    s_tx = heap_caps_aligned_calloc(DMA_ALIGN, 1, HOSTED_MAX_PACKET + BLOCK, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    s_rx = heap_caps_aligned_calloc(DMA_ALIGN, 1, HOSTED_RX_BUF + BLOCK, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    s_card = calloc(1, sizeof(*s_card));
    if (!s_tx || !s_rx || !s_card) {
        return ESP_ERR_NO_MEM;
    }

    pulse_reset_normal_boot();

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDIO_SLOT;
    // essl reads its registers (lengths, interrupts) into small buffers that are not DMA-aligned; the SDMMC driver refuses
    // those unless it may bounce them through its own aligned buffer.
    host.flags = SDMMC_HOST_FLAG_4BIT | SDMMC_HOST_FLAG_ALLOC_ALIGNED_BUF;
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 4;
    slot.clk = P4_PIN_C6_SDIO_CLK;
    slot.cmd = P4_PIN_C6_SDIO_CMD;
    slot.d0 = P4_PIN_C6_SDIO_D0;
    slot.d1 = P4_PIN_C6_SDIO_D1;
    slot.d2 = P4_PIN_C6_SDIO_D2;
    slot.d3 = P4_PIN_C6_SDIO_D3;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    ESP_RETURN_ON_ERROR(sdmmc_host_init(), TAG, "host init");
    s_host_up = true;
    ESP_RETURN_ON_ERROR(sdmmc_host_init_slot(SDIO_SLOT, &slot), TAG, "slot init");

    esp_err_t err = ESP_FAIL;
    for (int i = 0; i < CARD_INIT_TRIES && err != ESP_OK; i++) {
        vTaskDelay(pdMS_TO_TICKS(BOOT_WAIT_MS));
        card_clear(); // a failed attempt leaves a half-filled card behind
        err = sdmmc_card_init(&host, s_card);
    }
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "no SDIO slave on the C6 lines (%s)", esp_err_to_name(err));
        return ESP_ERR_NOT_FOUND;
    }
    const essl_sdio_config_t cfg = {.card = s_card, .recv_buffer_size = SLAVE_RX_BUF};
    ESP_RETURN_ON_ERROR(essl_sdio_init_dev(&s_essl, &cfg), TAG, "essl dev");
    ESP_RETURN_ON_ERROR(essl_init(s_essl, ESSL_TIMEOUT_MS), TAG, "essl init");
    return ESP_OK;
}

static esp_err_t link_open_data_path(void)
{
    const esp_err_t err = essl_send_slave_intr(s_essl, OPEN_DATA_PATH, ESSL_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "open data path: %s", esp_err_to_name(err));
    }
    return err;
}

static esp_err_t link_send(const uint8_t *buf, size_t len, uint32_t timeout_ms)
{
    if (len > HOSTED_MAX_PACKET) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(s_tx, buf, len);
    const esp_err_t err = essl_send_packet(s_essl, s_tx, len, timeout_ms);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "send: %s", esp_err_to_name(err));
    }
    return err;
}

static uint32_t link_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static esp_err_t link_recv(uint8_t *buf, size_t cap, size_t *len, uint32_t timeout_ms)
{
    const size_t room = cap < HOSTED_RX_BUF ? cap : HOSTED_RX_BUF;
    const uint32_t start = link_now_ms();

    for (;;) {
        size_t got = 0;
        // wait_ms 0: one look at the slave's length register; the polling below yields between looks.
        const esp_err_t err = essl_get_packet(s_essl, s_rx, room, &got, 0);
        if (err == ESP_OK || err == ESP_ERR_NOT_FINISHED) { // NOT_FINISHED: the rest comes with the next receive
            memcpy(buf, s_rx, got);
            *len = got;
            return ESP_OK;
        }
        if (err != ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "receive: %s", esp_err_to_name(err));
            return err;
        }
        if (link_now_ms() - start >= timeout_ms) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(POLL_TICKS);
    }
}

static const hosted_link_t s_link = {
    .start = link_start,
    .open_data_path = link_open_data_path,
    .send = link_send,
    .recv = link_recv,
    .release = link_release,
    .now_ms = link_now_ms,
};

esp_err_t c6_hosted_detect(bool *present)
{
    return hosted_session_detect(&s_link, present);
}

esp_err_t c6_hosted_ota(const c6_hosted_source_t *source, char *err_detail, size_t err_cap)
{
    return hosted_session_ota(&s_link, source, err_detail, err_cap);
}
