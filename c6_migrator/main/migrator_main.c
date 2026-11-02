#include <stdbool.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_app_desc.h"
#include "esp_flash.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "c6_bundle.h"
#include "migrator_core.h"

static const char *TAG = "c6_migrator";

#define MAX_SLOTS 8

static esp_err_t f_read(uint32_t addr, void *buf, size_t len)
{
    return esp_flash_read(NULL, buf, addr, len);
}

static esp_err_t f_erase(uint32_t addr, size_t len)
{
    return esp_flash_erase_region(NULL, addr, len);
}

static esp_err_t f_write(uint32_t addr, const void *buf, size_t len)
{
    return esp_flash_write(NULL, buf, addr, len);
}

static esp_err_t f_verify_image(uint32_t addr, uint32_t room)
{
    const esp_partition_pos_t pos = {.offset = addr, .size = room};
    esp_image_metadata_t meta;

    return esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &pos, &meta);
}

static const migrator_flash_t s_flash = {
    .read = f_read, .erase = f_erase, .write = f_write, .verify_image = f_verify_image,
};

/* Never returns: the P4 sees the C6 not coming back and reports the install as failed. */
static void idle(const char *why)
{
    for (;;) {
        ESP_LOGE(TAG, "stopped: %s", why);
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

/* Hand the C6 back to the app in the other slot (the factory ESP-Hosted firmware) and restart. */
static void back_to_stock(const esp_partition_t *running, const char *why)
{
    ESP_LOGE(TAG, "%s: returning to the factory firmware", why);
    const esp_partition_t *other = esp_ota_get_next_update_partition(running);
    if (other && esp_ota_set_boot_partition(other) == ESP_OK) {
        esp_restart();
    }
    idle("the factory firmware cannot be selected either");
}

static size_t app_slots(migrator_slot_t *out, size_t cap)
{
    size_t n = 0;
    esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, NULL);

    for (; it && n < cap; it = esp_partition_next(it)) {
        const esp_partition_t *p = esp_partition_get(it);
        out[n++] = (migrator_slot_t) {
            .offset = p->address, .size = p->size
        };
    }
    esp_partition_iterator_release(it);
    return n;
}

static const esp_partition_t *partition_at(uint32_t offset)
{
    esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, NULL);
    const esp_partition_t *found = NULL;

    for (; it && !found; it = esp_partition_next(it)) {
        const esp_partition_t *p = esp_partition_get(it);
        if (p->address == offset) {
            found = p;
        }
    }
    esp_partition_iterator_release(it);
    return found;
}

void app_main(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    ESP_LOGI(TAG, "c6_migrator %s running from %s at 0x%" PRIx32, esp_app_get_description()->version,
             running ? running->label : "?", running ? running->address : 0);
    if (!running) {
        idle("running partition unknown");
    }

    esp_image_metadata_t meta;
    const esp_partition_pos_t pos = {.offset = running->address, .size = running->size};
    if (esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &pos, &meta) != ESP_OK) {
        back_to_stock(running, "own image unreadable");
    }

    const uint32_t header_rel = c6_bundle_header_offset(meta.image_len);
    uint8_t header[C6_BUNDLE_HEADER_LEN];
    c6_bundle_t bundle;
    if (header_rel + sizeof(header) > running->size ||
            esp_flash_read(NULL, header, running->address + header_rel, sizeof(header)) != ESP_OK ||
            c6_bundle_header_decode(header, sizeof(header), &bundle) != ESP_OK) {
        back_to_stock(running, "no valid install bundle after the image");
    }
    const uint32_t data_addr = running->address + header_rel + C6_BUNDLE_HEADER_LEN;
    const uint32_t used = header_rel + C6_BUNDLE_HEADER_LEN + c6_bundle_data_len(&bundle);
    if (used > running->size) {
        back_to_stock(running, "install bundle runs past the slot");
    }

    migrator_slot_t slots[MAX_SLOTS];
    const migrator_situation_t situation = {
        .running = {.offset = running->address, .size = running->size},
        .used = used,
        .slots = slots,
        .slot_count = app_slots(slots, MAX_SLOTS),
        .ptable_is_ours = migrator_ptable_is_ours(&s_flash, &bundle, data_addr),
    };
    migrator_slot_t to = {0};

    switch (migrator_decide(&situation, &to)) {
    case MIGRATOR_ALREADY_OURS:
        idle("our partition table is already in place; nothing to migrate");
        break;
    case MIGRATOR_STUCK:
        back_to_stock(running, "the transfer overlaps the install targets and no free slot can take it");
        break;
    case MIGRATOR_RELOCATE: {
        ESP_LOGW(TAG, "relocating %" PRIu32 " bytes to 0x%" PRIx32, used, to.offset);
        const esp_partition_t *target = partition_at(to.offset);
        if (!target || migrator_relocate(&s_flash, running->address, to, used) != ESP_OK ||
                esp_ota_set_boot_partition(target) != ESP_OK) {
            back_to_stock(running, "relocation failed");
        }
        esp_restart();
        break;
    }
    case MIGRATOR_INSTALL:
        if (migrator_check_blobs(&s_flash, &bundle, data_addr) != ESP_OK) {
            back_to_stock(running, "install bundle damaged");
        }
        ESP_LOGW(TAG, "installing: RCP, updater, bootloader, partition table");
        esp_err_t err = migrator_install(&s_flash, &bundle, data_addr);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "install failed: %s", esp_err_to_name(err));
            idle("install failed; power-cycle the board to retry");
        }
        ESP_LOGI(TAG, "install done, restarting into the new firmware");
        esp_restart();
        break;
    }
}
