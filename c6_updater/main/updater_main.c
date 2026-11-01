#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "driver/gpio.h"
#include "driver/spi_slave.h"
#include "bootloader_common.h"
#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_image_format.h"
#include "esp_flash_partitions.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "c6_board.h"
#include "rcp_sync_proto.h"
#include "updater_core.h"

static const char *TAG = "c6_updater";

#define SPI_HOST_ID    SPI2_HOST
#define FRAME_MAX_LEN  (RCP_SYNC_PROTO_HEADER_LEN + RCP_SYNC_PROTO_MAX_PAYLOAD + RCP_SYNC_PROTO_CRC_LEN)
#define REPLY_TIMEOUT_MS 1000
#define SPI_BUF_LEN    ((FRAME_MAX_LEN + 3) & ~3u)

static updater_core_t s_core;
static esp_ota_handle_t s_ota;
static bool s_ota_open;
static const esp_partition_t *s_target;
static WORD_ALIGNED_ATTR DMA_ATTR uint8_t s_rx[SPI_BUF_LEN];
static WORD_ALIGNED_ATTR DMA_ATTR uint8_t s_tx[SPI_BUF_LEN];

static const esp_partition_t *find_ota(int index)
{
    return esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_MIN + index, NULL);
}

static bool slot_image_valid(const esp_partition_t *p)
{
    esp_image_metadata_t meta;
    const esp_partition_pos_t pos = {.offset = p->address, .size = p->size};

    return esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &pos, &meta) == ESP_OK;
}

static updater_otadata_t otadata_selection(void)
{
    const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, NULL);
    esp_ota_select_entry_t entries[2];

    if (!part || esp_partition_read(part, 0, &entries[0], sizeof(entries[0])) != ESP_OK ||
            esp_partition_read(part, part->erase_size, &entries[1], sizeof(entries[1])) != ESP_OK) {
        return UPDATER_OTADATA_NONE;
    }
    int active = bootloader_common_get_active_otadata(entries);
    if (active < 0) {
        return UPDATER_OTADATA_NONE;
    }
    return ((entries[active].ota_seq - 1) % 2 == 0) ? UPDATER_OTADATA_SLOT_0 : UPDATER_OTADATA_SLOT_1;
}

/* Slot verification is slow (a SHA-256 over the image); the result is kept until the slots are written. */
static struct {
    bool known;
    bool valid[2];
} s_slots;

/* The slot the C6 bootloader boots (updater_core_choose_boot_slot mirrors it, including its fall-forward when
 * otadata is blank after a ROM recovery). Never the running `test` partition. */
static const esp_partition_t *boot_slot(void)
{
    if (!s_slots.known) {
        s_slots.valid[0] = slot_image_valid(find_ota(0));
        s_slots.valid[1] = slot_image_valid(find_ota(1));
        s_slots.known = true;
    }
    return find_ota(updater_core_choose_boot_slot(otadata_selection(), s_slots.valid[0], s_slots.valid[1]));
}

static const esp_partition_t *other_slot(void)
{
    return boot_slot() == find_ota(0) ? find_ota(1) : find_ota(0);
}

static esp_err_t flash_slot_version(bool boot, char *out, size_t len)
{
    const esp_partition_t *p = boot ? boot_slot() : other_slot();
    esp_app_desc_t desc;

    if (!s_slots.valid[p == find_ota(0) ? 0 : 1] || esp_ota_get_partition_description(p, &desc) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    strlcpy(out, desc.version, len);
    return ESP_OK;
}

static esp_err_t flash_begin(uint32_t image_size)
{
    if (s_ota_open) {
        /* P4 restarted the session mid-image: drop the old handle first. */
        esp_ota_abort(s_ota);
        s_ota_open = false;
    }
    s_target = other_slot();
    s_slots.known = false; /* the target is erased from here on */
    ESP_RETURN_ON_ERROR(esp_ota_begin(s_target, image_size, &s_ota), TAG, "ota begin");
    s_ota_open = true;
    return ESP_OK;
}

static esp_err_t flash_write(uint32_t offset, const void *data, size_t len)
{
    (void)offset; /* the core guarantees sequential offsets */
    return esp_ota_write(s_ota, data, len);
}

static esp_err_t flash_finish(void)
{
    s_ota_open = false; /* esp_ota_end releases the handle whether or not it succeeds */
    s_slots.known = false;
    ESP_RETURN_ON_ERROR(esp_ota_end(s_ota), TAG, "image verification failed");
    return esp_ota_set_boot_partition(s_target);
}

static updater_flash_t s_flash = {
    .slot_version = flash_slot_version,
    .begin = flash_begin,
    .write = flash_write,
    .finish = flash_finish,
};

static void start_spi_slave(void);

static void int_set_ready(bool ready)
{
    gpio_set_level(C6_PIN_INT, ready ? 0 : 1);
}

/* Queue one slave transaction, signal readiness on INT, and wait until the master has clocked it. */
static esp_err_t run_transaction(spi_slave_transaction_t *t, TickType_t timeout)
{
    spi_slave_transaction_t *done;

    ESP_RETURN_ON_ERROR(spi_slave_queue_trans(SPI_HOST_ID, t, portMAX_DELAY), TAG, "queue");
    int_set_ready(true);
    esp_err_t err = spi_slave_get_trans_result(SPI_HOST_ID, &done, timeout);
    int_set_ready(false);
    if (err == ESP_ERR_TIMEOUT) {
        /* The driver cannot cancel a queued transaction: rebuild the slave to abandon it. */
        spi_slave_free(SPI_HOST_ID);
        start_spi_slave();
    }
    return err;
}

static void start_spi_slave(void)
{
    const spi_bus_config_t bus = {
        .mosi_io_num = C6_PIN_MOSI,
        .miso_io_num = C6_PIN_MISO,
        .sclk_io_num = C6_PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = SPI_BUF_LEN,
    };
    const spi_slave_interface_config_t slave = {
        .spics_io_num = C6_PIN_CS,
        .flags = 0,
        .queue_size = 1,
        .mode = 0,
    };

    ESP_ERROR_CHECK(spi_slave_initialize(SPI_HOST_ID, &bus, &slave, SPI_DMA_CH_AUTO));
}

static void init_int_line(void)
{
    /* Latch INT high before enabling the output so it never glitches low. */
    ESP_ERROR_CHECK(gpio_reset_pin(C6_PIN_INT));
    int_set_ready(false);
    ESP_ERROR_CHECK(gpio_set_direction(C6_PIN_INT, GPIO_MODE_OUTPUT));
}

void app_main(void)
{
    const char *version = esp_app_get_description()->version;

    const esp_partition_t *boot = boot_slot();
    const esp_partition_t *other = other_slot();

    if (!boot || !other) {
        ESP_LOGE(TAG, "OTA slots ota_0/ota_1 not found in the partition table");
        abort();
    }
    ESP_LOGI(TAG, "c6_updater %s ready (boot slot %s, inactive slot %s)", version, boot->label, other->label);
    s_flash.slot_size = other->size;
    updater_core_init(&s_core, &s_flash, version);
    init_int_line();
    start_spi_slave();

    for (;;) {
        spi_slave_transaction_t rx = {.length = SPI_BUF_LEN * 8, .rx_buffer = s_rx, .tx_buffer = s_tx};
        memset(s_tx, 0, sizeof(s_tx));
        if (run_transaction(&rx, portMAX_DELAY) != ESP_OK) {
            continue;
        }
        size_t reply_len = 0;
        esp_err_t err = updater_core_handle(&s_core, s_rx, rx.trans_len / 8, s_tx, sizeof(s_tx), &reply_len);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "request dropped: %s", esp_err_to_name(err));
            continue;
        }
        spi_slave_transaction_t tx = {.length = SPI_BUF_LEN * 8, .rx_buffer = s_rx, .tx_buffer = s_tx};
        if (run_transaction(&tx, pdMS_TO_TICKS(REPLY_TIMEOUT_MS)) != ESP_OK) {
            ESP_LOGW(TAG, "reply not clocked; dropped");
        }
    }
}
