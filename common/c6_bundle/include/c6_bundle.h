#pragma once

// The install bundle that the P4 appends to c6_migrator when it replaces the factory ESP-Hosted firmware of the C6: one
// ESP-Hosted OTA transfer carries the migrator app, then (at the next 4 KiB boundary) this header, then the blobs back to
// back in kind order. The ESP-Hosted OTA verifies only the app image; the bytes after it stay in the slot unchecked, and the
// migrator verifies them against this header. Pure: shared by the P4 (encode) and c6_migrator (decode).

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The factory ESP-Hosted layout of the C6 (esp-hosted-mcu slave/partitions.esp32c6.csv, 4 MB). The ESP-Hosted OTA writes into
 * the slot the C6 does not run: stock ota_1 on a C6 that was never updated. */
#define C6_BUNDLE_STOCK_OTA0_OFFSET 0x10000
#define C6_BUNDLE_STOCK_OTA1_OFFSET 0x190000
#define C6_BUNDLE_STOCK_SLOT_SIZE   0x180000

/* Everything the migrator writes lies below stock ota_1, so it never overwrites a transfer that landed there. */
#define C6_BUNDLE_TARGET_END        C6_BUNDLE_STOCK_OTA1_OFFSET

#define C6_BUNDLE_ALIGN      0x1000
#define C6_BUNDLE_MAGIC      "OTBRMIG1"
#define C6_BUNDLE_MAGIC_LEN  8
#define C6_BUNDLE_VERSION    1
#define C6_BUNDLE_SHA256_LEN 32

typedef enum {
    C6_BUNDLE_BOOTLOADER = 1, /**< written at C6_FLASH_BOOTLOADER_OFFSET */
    C6_BUNDLE_PTABLE,         /**< written at C6_FLASH_PTABLE_OFFSET */
    C6_BUNDLE_UPDATER,        /**< written at C6_FLASH_TEST_OFFSET */
    C6_BUNDLE_RCP,            /**< written at C6_FLASH_OTA0_OFFSET */
} c6_bundle_kind_t;

#define C6_BUNDLE_ENTRIES 4

typedef struct {
    uint32_t kind; /**< c6_bundle_kind_t; entry i has kind i + 1 */
    uint32_t length;
    uint8_t sha256[C6_BUNDLE_SHA256_LEN];
} c6_bundle_entry_t;

typedef struct {
    c6_bundle_entry_t entries[C6_BUNDLE_ENTRIES];
} c6_bundle_t;

/** Encoded size: magic, version, count, the entries (kind, length, hash), the SHA-256 of everything before it. */
#define C6_BUNDLE_HEADER_LEN (C6_BUNDLE_MAGIC_LEN + 4 + 4 + C6_BUNDLE_ENTRIES * (4 + 4 + C6_BUNDLE_SHA256_LEN) + C6_BUNDLE_SHA256_LEN)

/** Flash offset the blob of `kind` goes to; 0 for an unknown kind (the bootloader's offset is 0 too: check the kind first). */
uint32_t c6_bundle_target_offset(c6_bundle_kind_t kind);

/** Room the blob of `kind` has at its target; 0 for an unknown kind. */
uint32_t c6_bundle_target_room(c6_bundle_kind_t kind);

/** Where the bundle header starts after an app image of `image_len` bytes: the next C6_BUNDLE_ALIGN boundary. */
uint32_t c6_bundle_header_offset(uint32_t image_len);

/** Sum of the blob lengths (the data that follows the header). */
uint32_t c6_bundle_data_len(const c6_bundle_t *bundle);

/**
 * Write the header (C6_BUNDLE_HEADER_LEN bytes). ESP_ERR_INVALID_ARG for NULL; ESP_ERR_INVALID_SIZE for a bundle that
 * decode would refuse (wrong kind order, an empty blob, a blob larger than its room).
 */
esp_err_t c6_bundle_header_encode(const c6_bundle_t *bundle, uint8_t *out, size_t out_cap);

/**
 * Parse and check a header. ESP_ERR_INVALID_ARG for NULL; ESP_ERR_INVALID_SIZE if len is short, a blob is empty, out of kind
 * order or larger than its room; ESP_ERR_NOT_SUPPORTED for a wrong magic or format version; ESP_ERR_INVALID_CRC when the
 * header hash does not match. `out` is written only on ESP_OK.
 */
esp_err_t c6_bundle_header_decode(const uint8_t *buf, size_t len, c6_bundle_t *out);

/** Incremental SHA-256 (PSA), for blobs read in chunks. */
typedef struct {
    void *op; /**< psa_hash_operation_t, heap-allocated by begin, freed by finish/abort */
} c6_bundle_sha256_t;

esp_err_t c6_bundle_sha256_begin(c6_bundle_sha256_t *ctx);
esp_err_t c6_bundle_sha256_update(c6_bundle_sha256_t *ctx, const void *data, size_t len);
/** Writes the digest and frees the context, also on error. */
esp_err_t c6_bundle_sha256_finish(c6_bundle_sha256_t *ctx, uint8_t out[C6_BUNDLE_SHA256_LEN]);
void c6_bundle_sha256_abort(c6_bundle_sha256_t *ctx);

/** One-shot SHA-256. */
esp_err_t c6_bundle_sha256(const void *data, size_t len, uint8_t out[C6_BUNDLE_SHA256_LEN]);

#ifdef __cplusplus
}
#endif
