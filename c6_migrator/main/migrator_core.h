#pragma once

// c6_migrator logic, pure: what to do from where the migrator runs, and the install of the bundle that follows it in its
// slot (common/c6_bundle). The flash is behind migrator_flash_t, so the host tests run it on a fake flash.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "c6_bundle.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Erase granularity of the C6 flash; every region the migrator erases starts on it. */
#define MIGRATOR_SECTOR 0x1000

/** A region of the C6 flash: an app partition of the foreign table, or what the transfer occupies in it. */
typedef struct {
    uint32_t offset;
    uint32_t size;
} migrator_slot_t;

typedef enum {
    MIGRATOR_INSTALL,      /**< the transfer is clear of every target: install from where it is */
    MIGRATOR_RELOCATE,     /**< the transfer overlaps a target (stock ota_0): copy it to a clear slot first */
    MIGRATOR_ALREADY_OURS, /**< our partition table is in place already: never touch anything */
    MIGRATOR_STUCK,        /**< it overlaps a target and no clear slot can take it */
} migrator_action_t;

typedef struct {
    migrator_slot_t running;     /**< the partition the migrator runs from */
    uint32_t used;               /**< bytes the transfer occupies from running.offset: image, padding, header, blobs */
    const migrator_slot_t *slots; /**< every app partition of the foreign table (the running one may be among them) */
    size_t slot_count;
    bool ptable_is_ours;         /**< 0x8000 already holds the bundle's partition table */
} migrator_situation_t;

/**
 * Decide from the situation. RELOCATE fills *relocate_to with the first app partition other than the running one that starts
 * at or after C6_BUNDLE_TARGET_END and holds `used` bytes.
 */
migrator_action_t migrator_decide(const migrator_situation_t *s, migrator_slot_t *relocate_to);

/** The flash as the migrator needs it. Addresses are absolute flash offsets. */
typedef struct {
    esp_err_t (*read)(uint32_t addr, void *buf, size_t len);
    /** Erase [addr, addr + len); both are multiples of MIGRATOR_SECTOR. */
    esp_err_t (*erase)(uint32_t addr, size_t len);
    esp_err_t (*write)(uint32_t addr, const void *buf, size_t len);
    /** Full app image verification (esp_image_verify) of the image at addr within `room` bytes. */
    esp_err_t (*verify_image)(uint32_t addr, uint32_t room);
} migrator_flash_t;

/** Flash address of the blob of `kind`, given where the blob data starts (right after the header). */
uint32_t migrator_blob_addr(const c6_bundle_t *bundle, uint32_t data_addr, c6_bundle_kind_t kind);

/** SHA-256 of every blob, read from flash, against the header. ESP_ERR_INVALID_CRC on the first mismatch. */
esp_err_t migrator_check_blobs(const migrator_flash_t *flash, const c6_bundle_t *bundle, uint32_t data_addr);

/** True when the flash at C6_FLASH_PTABLE_OFFSET equals the bundle's partition table. Errors read as false. */
bool migrator_ptable_is_ours(const migrator_flash_t *flash, const c6_bundle_t *bundle, uint32_t data_addr);

/**
 * Install the bundle: RCP to ota_0, updater to test, bootloader, partition table (in that order), then erase our otadata.
 * A region already holding its blob is skipped. Otherwise it is erased, written and verified (byte compare, plus an image
 * verification for the RCP and the updater); a failure gets one more try, the second failure ends the install and later
 * regions are not touched. Call migrator_check_blobs first. The caller restarts the C6 after ESP_OK.
 */
esp_err_t migrator_install(const migrator_flash_t *flash, const c6_bundle_t *bundle, uint32_t data_addr);

/**
 * Copy `len` bytes from `from` to the start of slot `to` (erased first, rounded up to whole sectors) and compare them. Does
 * not select the slot: the caller does, then restarts. ESP_ERR_INVALID_SIZE if `len` does not fit `to`.
 */
esp_err_t migrator_relocate(const migrator_flash_t *flash, uint32_t from, migrator_slot_t to, uint32_t len);

#ifdef __cplusplus
}
#endif
