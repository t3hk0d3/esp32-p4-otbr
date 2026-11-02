#include <string.h>
#include "installer_core.h"

#define CHUNK           INSTALLER_SECTOR
#define REGION_ATTEMPTS 2

static uint8_t s_a[CHUNK];
static uint8_t s_b[CHUNK];

static uint32_t round_up(uint32_t v)
{
    return (v + INSTALLER_SECTOR - 1) & ~(uint32_t)(INSTALLER_SECTOR - 1);
}

installer_action_t installer_decide(const installer_situation_t *s, installer_slot_t *relocate_to)
{
    if (s->ptable_is_ours) {
        return INSTALLER_ALREADY_OURS;
    }
    if (s->running.offset >= INSTALLER_TARGET_END) {
        return INSTALLER_INSTALL; // every target lies in [0, INSTALLER_TARGET_END)
    }
    for (size_t i = 0; i < s->slot_count; i++) {
        const installer_slot_t *c = &s->slots[i];
        if (c->offset != s->running.offset && c->offset >= INSTALLER_TARGET_END && c->size >= round_up(s->image_len)) {
            *relocate_to = *c;
            return INSTALLER_RELOCATE;
        }
    }
    return INSTALLER_STUCK;
}

static bool blob_fits(const installer_blob_t *b, uint32_t room)
{
    return b->data && b->len > 0 && b->len <= room;
}

esp_err_t installer_images_check(const installer_images_t *images)
{
    if (!images || !blob_fits(&images->bootloader, INSTALLER_BOOTLOADER_ROOM) ||
            !blob_fits(&images->ptable, INSTALLER_PTABLE_ROOM) || !blob_fits(&images->updater, C6_FLASH_TEST_SIZE)) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

/* Byte compare of flash [addr, addr + len) with memory. ESP_ERR_INVALID_CRC on a difference. */
static esp_err_t compare_mem(const installer_flash_t *flash, uint32_t addr, const uint8_t *data, uint32_t len)
{
    for (uint32_t done = 0; done < len; done += CHUNK) {
        const size_t n = len - done < CHUNK ? len - done : CHUNK;
        esp_err_t err = flash->read(addr + done, s_a, n);
        if (err != ESP_OK) {
            return err;
        }
        if (memcmp(s_a, data + done, n) != 0) {
            return ESP_ERR_INVALID_CRC;
        }
    }
    return ESP_OK;
}

/* Byte compare of two flash ranges. */
static esp_err_t compare_flash(const installer_flash_t *flash, uint32_t a, uint32_t b, uint32_t len)
{
    for (uint32_t done = 0; done < len; done += CHUNK) {
        const size_t n = len - done < CHUNK ? len - done : CHUNK;
        esp_err_t err = flash->read(b + done, s_b, n);
        if (err != ESP_OK) {
            return err;
        }
        err = compare_mem(flash, a + done, s_b, n);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}

bool installer_ptable_is_ours(const installer_flash_t *flash, const installer_images_t *images)
{
    return images->ptable.data &&
           compare_mem(flash, C6_FLASH_PTABLE_OFFSET, images->ptable.data, images->ptable.len) == ESP_OK;
}

static esp_err_t place(const installer_flash_t *flash, uint32_t dst, const installer_blob_t *blob, uint32_t image_room)
{
    if (compare_mem(flash, dst, blob->data, blob->len) == ESP_OK) {
        return ESP_OK; // already in place (a repeat after a power cut)
    }
    esp_err_t err = ESP_FAIL;
    for (int attempt = 0; attempt < REGION_ATTEMPTS; attempt++) {
        err = flash->erase(dst, round_up(blob->len));
        if (err == ESP_OK) {
            err = flash->write(dst, blob->data, blob->len);
        }
        if (err == ESP_OK) {
            err = compare_mem(flash, dst, blob->data, blob->len);
        }
        if (err == ESP_OK && image_room) {
            err = flash->verify_image(dst, image_room);
        }
        if (err == ESP_OK) {
            return ESP_OK;
        }
    }
    return err;
}

esp_err_t installer_install(const installer_flash_t *flash, const installer_images_t *images)
{
    // The updater first: until the bootloader and the partition table change, the stock ones still boot this installer, so a
    // power cut there only repeats the run. The bootloader goes before the partition table: with our bootloader and the stock
    // table the C6 still boots the installer.
    esp_err_t err = place(flash, C6_FLASH_TEST_OFFSET, &images->updater, C6_FLASH_TEST_SIZE);
    if (err == ESP_OK) {
        err = place(flash, C6_FLASH_BOOTLOADER_OFFSET, &images->bootloader, 0);
    }
    if (err == ESP_OK) {
        err = place(flash, C6_FLASH_PTABLE_OFFSET, &images->ptable, 0);
    }
    if (err == ESP_OK) {
        // Leftover ESP-Hosted bytes at ota_0 must not look like an app; our bootloader then needs GPIO6 low to boot anything.
        err = flash->erase(C6_FLASH_OTA0_OFFSET, INSTALLER_SECTOR);
    }
    if (err == ESP_OK) {
        err = flash->erase(C6_FLASH_OTADATA_OFFSET, C6_FLASH_OTADATA_SIZE);
    }
    return err;
}

esp_err_t installer_relocate(const installer_flash_t *flash, uint32_t from, installer_slot_t to, uint32_t len)
{
    if (len == 0 || round_up(len) > to.size) {
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t err = flash->erase(to.offset, round_up(len));
    for (uint32_t done = 0; err == ESP_OK && done < len; done += CHUNK) {
        const size_t n = len - done < CHUNK ? len - done : CHUNK;
        err = flash->read(from + done, s_a, n);
        if (err == ESP_OK) {
            err = flash->write(to.offset + done, s_a, n);
        }
    }
    return err == ESP_OK ? compare_flash(flash, to.offset, from, len) : err;
}
