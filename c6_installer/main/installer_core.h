#pragma once

// c6_installer logic, pure: what to do from where the installer runs, and the install of the three images it embeds (our
// bootloader, partition table and updater). The flash is behind installer_flash_t, so the host tests run it on a fake flash.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "c6_board.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Erase granularity of the C6 flash; every region the installer erases starts on it. */
#define INSTALLER_SECTOR 0x1000

/** Everything the install writes or erases lies below this: up to the end of the updater (test) and the first ota_0 sector. */
#define INSTALLER_TARGET_END (C6_FLASH_OTA0_OFFSET + INSTALLER_SECTOR)

/** Factory ESP-Hosted layout (slave/partitions.esp32c6.csv): the two app slots the installer can run from. */
#define INSTALLER_STOCK_OTA0_OFFSET 0x10000
#define INSTALLER_STOCK_OTA1_OFFSET 0x190000
#define INSTALLER_STOCK_SLOT_SIZE   0x180000

/** Room of each destination: the bootloader ends where the partition table starts, the table where nvs starts. */
#define INSTALLER_BOOTLOADER_ROOM (C6_FLASH_PTABLE_OFFSET - C6_FLASH_BOOTLOADER_OFFSET)
#define INSTALLER_PTABLE_ROOM     (C6_FLASH_NVS_OFFSET - C6_FLASH_PTABLE_OFFSET)

/** A region of the C6 flash: an app partition of the foreign table, or the one the installer runs from. */
typedef struct {
    uint32_t offset;
    uint32_t size;
} installer_slot_t;

typedef struct {
    const uint8_t *data;
    uint32_t len;
} installer_blob_t;

/** The images the installer carries (EMBED_FILES of the c6_updater build). */
typedef struct {
    installer_blob_t bootloader;
    installer_blob_t ptable;
    installer_blob_t updater;
} installer_images_t;

typedef enum {
    INSTALLER_INSTALL,      /**< the running image is clear of every target: install from where it is */
    INSTALLER_RELOCATE,     /**< it overlaps a target (stock ota_0): copy it to a clear slot first */
    INSTALLER_ALREADY_OURS, /**< our partition table is in place already: never touch anything */
    INSTALLER_STUCK,        /**< it overlaps a target and no clear slot can take it */
} installer_action_t;

typedef struct {
    installer_slot_t running;      /**< the partition the installer runs from */
    uint32_t image_len;            /**< length of the installer's own image */
    const installer_slot_t *slots; /**< every app partition of the foreign table (the running one may be among them) */
    size_t slot_count;
    bool ptable_is_ours;           /**< 0x8000 already holds the embedded partition table */
} installer_situation_t;

/**
 * Decide from the situation. RELOCATE fills *relocate_to with the first app partition other than the running one that starts
 * at or after INSTALLER_TARGET_END and holds the image.
 */
installer_action_t installer_decide(const installer_situation_t *s, installer_slot_t *relocate_to);

/** The flash as the installer needs it. Addresses are absolute flash offsets. */
typedef struct {
    esp_err_t (*read)(uint32_t addr, void *buf, size_t len);
    /** Erase [addr, addr + len); both are multiples of INSTALLER_SECTOR. */
    esp_err_t (*erase)(uint32_t addr, size_t len);
    esp_err_t (*write)(uint32_t addr, const void *buf, size_t len);
    /** Full app image verification (esp_image_verify) of the image at addr within `room` bytes. */
    esp_err_t (*verify_image)(uint32_t addr, uint32_t room);
} installer_flash_t;

/** Every image present and within its destination's room. ESP_ERR_INVALID_SIZE otherwise. */
esp_err_t installer_images_check(const installer_images_t *images);

/** True when the flash at C6_FLASH_PTABLE_OFFSET equals the embedded partition table. Errors read as false. */
bool installer_ptable_is_ours(const installer_flash_t *flash, const installer_images_t *images);

/**
 * Install: updater to test, bootloader, partition table (in that order), then erase the first sector of ota_0 and our
 * otadata. A region already holding its image is skipped. Otherwise it is erased, written and verified (byte compare, plus an
 * image verification for the updater); a failure gets one more try, the second failure ends the install and later regions are
 * not touched. Call installer_images_check first. The caller restarts the C6 after ESP_OK.
 */
esp_err_t installer_install(const installer_flash_t *flash, const installer_images_t *images);

/**
 * Copy `len` bytes from flash `from` to the start of slot `to` (erased first, rounded up to whole sectors) and compare them.
 * Does not select the slot: the caller does, then restarts. ESP_ERR_INVALID_SIZE if `len` does not fit `to`.
 */
esp_err_t installer_relocate(const installer_flash_t *flash, uint32_t from, installer_slot_t to, uint32_t len);

#ifdef __cplusplus
}
#endif
