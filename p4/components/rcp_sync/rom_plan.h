#pragma once

// ROM recovery as data (spec 5.10): which flash regions are written or erased, in which order, and the loop that
// runs them with verification and one retry per region. Pure: the flasher is behind rom_ops_t, so the host tests run it.

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ROM_PLAN_MAX_REGIONS 4
#define ROM_REGION_ATTEMPTS  3 /* the link over the jumper can be flaky (Phase 0, Task 22): verify, retry twice at lower baud rates */

/** The baud rate ladder: the first try of a run uses ROM_BAUD_LADDER[0], each retry the next lower one (reconnect included). */
#define ROM_BAUD_LADDER_LEN  3

typedef enum {
    ROM_REGION_WRITE,
    ROM_REGION_ERASE,
} rom_region_kind_t;

typedef struct {
    const char *name;
    rom_region_kind_t kind;
    uint32_t offset;
    uint32_t size;
    const uint8_t *data; /**< WRITE only; `size` bytes */
} rom_region_t;

typedef struct {
    rom_region_t regions[ROM_PLAN_MAX_REGIONS];
    size_t count;
} rom_plan_t;

/**
 * The base image plan: bootloader, partition table, erase otadata, updater to `test`, in that order. Every size is
 * checked against the room its region has (bootloader up to the partition table, the table up to nvs, the updater
 * up to the end of `test`); the RCP slots are never part of it. Nothing is touched here: on error plan->count is 0.
 * ESP_ERR_INVALID_ARG for a NULL pointer, ESP_ERR_INVALID_SIZE for an empty or oversized blob.
 */
esp_err_t rom_plan_build_base(rom_plan_t *plan, const uint8_t *bootloader, size_t bootloader_len, const uint8_t *ptable,
                              size_t ptable_len, const uint8_t *updater, size_t updater_len);

/** A plan that erases one range (the test command c6_erase). ESP_ERR_INVALID_ARG unless offset and size are 4 KiB aligned and size > 0. */
esp_err_t rom_plan_build_erase(rom_plan_t *plan, const char *name, uint32_t offset, uint32_t size);

/**
 * The 16-byte MD5 digest as the 32 lowercase hex characters (plus the terminator) that the ROM loader's MD5 command
 * returns and esp_loader_flash_verify_known_md5 compares against.
 */
void rom_plan_md5_hex(const uint8_t digest[16], char out[33]);

/**
 * Baud rate of the n-th rung of the ladder: 1500000, 460800, 115200; rungs past the last stay at 115200. The run starts at
 * rung 0 and goes one rung down on every retry, and stays there for the regions after it (a link that failed once is not
 * trusted again within the same recovery).
 */
uint32_t rom_plan_baud_for_rung(unsigned rung);

/** What the loop needs from the flasher. */
typedef struct {
    /**
     * Enter ROM download from scratch (reset the C6 into its loader, sync at the ROM's default 115200 baud), check the target is an
     * ESP32-C6 (ESP_ERR_NOT_SUPPORTED otherwise), then switch both ends to `baud` (115200 = leave it). Called again with a lower
     * baud rate before every retry: a failed transfer can leave the loader or the UART out of step, a fresh connect always
     * resynchronises. The UART and pins are set up by the first call only.
     */
    esp_err_t (*connect)(uint32_t baud);
    /** Write or erase one region. */
    esp_err_t (*write_region)(const rom_region_t *region);
    /** Check one region after write_region (MD5 of what was written, or of erased flash). */
    esp_err_t (*verify_region)(const rom_region_t *region);
    /** Leave JUMPER_BOOT high-impedance, reset the C6 into its normal boot, free the UART. Called exactly once, on every path. */
    void (*release)(void);
} rom_ops_t;

/**
 * connect (rung 0), then every region in order: write, verify, and on a failure of either one more try of both after a
 * fresh connect one rung down the baud ladder; the first region that fails ROM_REGION_ATTEMPTS times, or a reconnect that
 * fails, ends the run (later regions are not touched). release is called once whatever happens. Returns the error of the
 * failing step. Worst case (every try of the 228 KB updater fails late): 3 transfers (~5 s, ~10 s, ~25 s) plus 3 connects
 * (a few seconds each) for that region, about a minute; the small regions add seconds, so a recovery is bounded to ~2 minutes.
 */
esp_err_t rom_plan_run(const rom_plan_t *plan, const rom_ops_t *ops);

#ifdef __cplusplus
}
#endif
