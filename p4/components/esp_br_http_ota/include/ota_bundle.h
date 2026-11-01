#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OTA_BUNDLE_MAGIC 0x4242544FU /* "OTBB" read as little-endian bytes 'O' 'T' 'B' 'B' */
#define OTA_BUNDLE_FORMAT_VERSION 1
#define OTA_BUNDLE_HEADER_LEN 44

/** esp_br_ota_begin refusal: the running image is still pending verification (first 60 s after an update). */
#define ESP_BR_OTA_ERR_NOT_CONFIRMED 0x7b01

/** Bundle header (spec 5.6), little-endian on the wire: header, web image (trailing 0xFF trimmed), P4 app image. */
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t format_version;
    uint16_t reserved;
    uint32_t web_len;
    uint8_t web_sha256[32];
} ota_bundle_header_t;

_Static_assert(sizeof(ota_bundle_header_t) == OTA_BUNDLE_HEADER_LEN, "bundle header is 44 bytes");

/**
 * Parse the first OTA_BUNDLE_HEADER_LEN bytes of a bundle into host byte order.
 *
 * @return ESP_OK; ESP_ERR_INVALID_ARG for NULL arguments; ESP_ERR_INVALID_SIZE if `len` is shorter than the header;
 *         ESP_ERR_INVALID_VERSION for a wrong magic or format_version. `out` is only written on ESP_OK.
 */
esp_err_t ota_bundle_parse_header(const uint8_t *buf, size_t len, ota_bundle_header_t *out);

#ifdef __cplusplus
}
#endif
