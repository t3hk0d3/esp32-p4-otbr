#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Outcome of the boot-time probe of the C6 updater (spec 5.3 step 3). */
typedef struct {
    char installed_version[32]; /**< version of the C6 boot slot; empty when installed_valid is false */
    bool installed_valid;       /**< the boot slot holds an RCP image (its version is not empty) */
    bool updater_reachable;     /**< the updater answered HELLO */
    bool jumper_present;        /**< the C6 ROM boot banner was seen on JUMPER_RX since boot */
    bool rom_recovered;         /**< this probe ran a ROM recovery that succeeded (the fields describe the C6 afterwards) */
    bool rom_recovery_failed;   /**< this probe ran a ROM recovery that failed; the other fields then describe the C6 as probed before it */
    char updater_version[32];   /**< version of the C6 base image (updater), when reachable */
    bool esp_hosted;            /**< the updater was silent, no jumper, and the factory ESP-Hosted firmware answered */
    char last_error[64];        /**< why the probe failed, "" otherwise */
} rcp_sync_probe_result_t;

/** last_error of a probe that found the factory ESP-Hosted firmware on the C6. */
#define RCP_SYNC_ESP_HOSTED_ERROR "esp-hosted firmware"

/** Project name of the one-time C6 installer image (c6_installer.bin) in its app description. */
#define RCP_SYNC_INSTALLER_PROJECT "c6_installer"

/**
 * Probe the installed RCP firmware (spec 5.3 step 3): put the C6 into its updater, HELLO with the normal
 * retries, read the boot slot version, record it for rcp_sync_get_last_probe(). Sends no BEGIN. Always releases the
 * SPI bus and leaves GPIO6 high; the caller performs the hardware reset (rcp_sync_boot_rcp) afterwards.
 * A silent or failing updater is reported in out (updater_reachable false, last_error set) and returns ESP_OK,
 * so a failed probe never blocks the boot. ESP_ERR_INVALID_ARG if out is NULL, ESP_ERR_NO_MEM.
 *
 * Maintenance jumper (spec 5.3 step 3, 5.10): a C6 ROM banner seen during the reset sets jumper_present. With the
 * jumper present, a silent updater (after the 3 entries) or an updater_version other than the embedded base image's
 * triggers a ROM recovery (tens of seconds), at most once per boot, then the probe runs again; out describes the C6
 * afterwards (rom_recovered / rom_recovery_failed tell what happened).
 *
 * Without the jumper, a silent updater is followed by a look for the factory ESP-Hosted firmware on SDIO (esp_hosted,
 * last_error RCP_SYNC_ESP_HOSTED_ERROR); nothing is written to the C6 then.
 */
esp_err_t rcp_sync_probe(rcp_sync_probe_result_t *out);

/** Copy of the result of the most recent rcp_sync_probe (all zero before the first run). Safe from any task. */
void rcp_sync_get_last_probe(rcp_sync_probe_result_t *out);

/** rcp_sync_push_firmware result: the C6 refused the complete image at END (verification failed). Not retried. */
#define RCP_SYNC_ERR_IMAGE_REJECTED 0x7a01

/** rcp_sync_push_firmware result: sending c6_installer.bin through the ESP-Hosted OTA failed (err_detail names the step). */
#define RCP_SYNC_ERR_HOSTED_OTA 0x7a02

/**
 * Radio push result (br_core): the C6 installed the image, but OpenThread did not come back up on it (err_detail
 * names the fault). Pushing the same image again does not help.
 */
#define RCP_SYNC_ERR_RESTART 0x7a03

/**
 * Where an RCP image comes from during a push. `read` delivers bytes sequentially: offsets ascend without
 * gaps. A NULL `reopen` marks a one-shot source (a request body). `reopen` is non-NULL only for sources that
 * can restart from offset 0 (a URL pull); it is called before every whole-transfer retry.
 */
typedef struct {
    size_t total_size;
    esp_err_t (*read)(void *ctx, size_t offset, void *buf, size_t len);
    esp_err_t (*reopen)(void *ctx);
    void *ctx;
} rcp_fw_source_t;

/**
 * Push an RCP image to the C6 updater: enter the updater, HELLO, validate total_size (0 or above the slot
 * size: ESP_ERR_INVALID_SIZE, no BEGIN), then BEGIN/DATA/END. A re-openable source gets 3 whole-transfer
 * attempts, a one-shot source 1; a verification error at END ends the attempts. A frame NAK or timeout resends
 * the buffered frame, never re-reading the source. Only END switches the boot slot, so a failed transfer leaves
 * the previous firmware booting. Always releases the SPI bus and leaves GPIO6 high; it does not boot the RCP
 * (the caller resets the C6, rcp_sync_boot_rcp). err_detail (may be NULL) receives a short reason on failure,
 * "" on success. An image of RCP_SYNC_INSTALLER_PROJECT is refused (RCP_SYNC_ERR_IMAGE_REJECTED, no BEGIN).
 *
 * First install (no jumper, the updater silent on the first attempt, the factory ESP-Hosted firmware answering): the
 * source must be c6_installer.bin of this release (project RCP_SYNC_INSTALLER_PROJECT, version equal to the embedded
 * base version), else RCP_SYNC_ERR_IMAGE_REJECTED and nothing is sent. It goes unchanged through the ESP-Hosted OTA
 * (a refusal: RCP_SYNC_ERR_HOSTED_OTA); the C6 restarts into it, it installs the base image and restarts into the
 * updater, which must answer HELLO within about 3 min (ESP_ERR_TIMEOUT otherwise). The C6 is never reset meanwhile, also
 * not after an OTA timeout (the C6 may have taken the image): the P4 waits for the updater then too.
 * ESP_OK then, with *base_only (may be NULL; false on every other outcome) true: the C6 has its updater and no RCP yet.
 */
esp_err_t rcp_sync_push_firmware(const rcp_fw_source_t *source, char *err_detail, size_t err_cap, bool *base_only);

/** Drive GPIO6 high and pulse GPIO54: the C6 boots its boot slot. */
esp_err_t rcp_sync_boot_rcp(void);

/**
 * The C6 base image embedded in the app (ROM recovery source). The blobs belong to main (EMBED_FILES), hence this setter:
 * call it once before the first probe. Pointers must stay valid (embedded data); the version text has no terminator.
 * Without it no recovery is ever attempted. ESP_ERR_INVALID_ARG for a NULL image, a NULL pointer or an empty/negative range
 * (nothing is stored then).
 */
typedef struct {
    const uint8_t *bootloader_start, *bootloader_end;
    const uint8_t *ptable_start, *ptable_end;
    const uint8_t *updater_start, *updater_end;
    const uint8_t *version_start, *version_end;
} rcp_sync_base_image_t;

esp_err_t rcp_sync_set_base_image(const rcp_sync_base_image_t *image);

/** Size of the C6 console ring buffer (spec 5.10). */
#define RCP_SYNC_LOG_CAPACITY (16 * 1024)

/**
 * True once the C6 ROM boot banner was seen on JUMPER_RX (the maintenance jumper is wired); latched until reboot.
 * Detection happens while the probe resets the C6 into its updater, so it is false before the first probe.
 */
bool rcp_sync_jumper_present(void);

/**
 * ROM recovery of the C6 base image over the jumper (spec 5.10): ESP_ERR_NOT_SUPPORTED without the jumper (no pin is
 * touched then). Otherwise: target must be an ESP32-C6, the embedded bootloader and partition table are written, otadata
 * is erased, the embedded updater is written to `test`, each region verified (MD5) with one retry. No RCP firmware is
 * written, ota_0 and ota_1 stay as they are. The C6 is reset into its normal boot afterwards. JUMPER_BOOT is left
 * high-impedance on every path, errors included. Blocks for tens of seconds; pauses the C6 log capture meanwhile.
 * ESP_ERR_INVALID_STATE if a recovery already runs. It does not stop OpenThread: use builtin_rcp_rom_recover.
 */
esp_err_t rcp_sync_rom_recover(void);

/** Copy of the newest C6 console output (at most RCP_SYNC_LOG_CAPACITY bytes), oldest first, no terminator. Returns the length. */
size_t rcp_sync_log_read(char *buf, size_t cap);

#ifdef OTBR_TEST_BUILD
/** Test builds: erase one C6 flash range over the jumper (c6_erase). Same preconditions and exit state as rcp_sync_rom_recover. */
esp_err_t rcp_sync_test_c6_erase(uint32_t offset, uint32_t size);

/** One image for rcp_sync_test_c6_write. */
typedef struct {
    const char *name;
    uint32_t offset; /**< 4 KiB aligned */
    const uint8_t *data;
    size_t len;
} rcp_sync_test_image_t;

/**
 * Test builds: write up to 4 images to the C6 over the jumper (ROM download), each erased, written and MD5-verified as by
 * esptool, then reset the C6 (c6_stock_restore). Same preconditions and exit state as rcp_sync_rom_recover.
 */
esp_err_t rcp_sync_test_c6_write(const rcp_sync_test_image_t *images, size_t count);

/**
 * Test builds: from now on (NVS, survives reboots) the probe and the push treat the jumper as absent (c6_jumper_ignore), so
 * the wireless first install runs on a board that has the jumper fitted. Manual ROM recovery still uses the jumper.
 */
esp_err_t rcp_sync_test_set_jumper_ignore(bool ignore);
#endif

#ifdef __cplusplus
}
#endif
