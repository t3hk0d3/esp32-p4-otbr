#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * C6 base image embedded in the P4 app for ROM recovery over the maintenance jumper. Each blob is
 * the byte range [start, end); `end` points one past the last byte. Built from the c6_updater
 * project by p4/CMakeLists.txt.
 */

/** C6 second-stage bootloader (flashed at 0x0). */
extern const uint8_t c6_base_bootloader_start[] asm("_binary_c6_bootloader_bin_start");
extern const uint8_t c6_base_bootloader_end[] asm("_binary_c6_bootloader_bin_end");

/** C6 partition table (flashed at 0x8000). */
extern const uint8_t c6_base_ptable_start[] asm("_binary_c6_partition_table_bin_start");
extern const uint8_t c6_base_ptable_end[] asm("_binary_c6_partition_table_bin_end");

/** Frozen C6 updater app. */
extern const uint8_t c6_base_updater_start[] asm("_binary_c6_updater_bin_start");
extern const uint8_t c6_base_updater_end[] asm("_binary_c6_updater_bin_end");

/** Updater PROJECT_VER as plain text, no terminating NUL (length is end - start). */
extern const uint8_t c6_base_version_start[] asm("_binary_c6_base_version_txt_start");
extern const uint8_t c6_base_version_end[] asm("_binary_c6_base_version_txt_end");

#ifdef __cplusplus
}
#endif
