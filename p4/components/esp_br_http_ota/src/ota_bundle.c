#include "ota_bundle.h"
#include <string.h>

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

esp_err_t ota_bundle_parse_header(const uint8_t *buf, size_t len, ota_bundle_header_t *out)
{
    if (!buf || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len < OTA_BUNDLE_HEADER_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }
    ota_bundle_header_t header;
    header.magic = le32(buf);
    header.format_version = le16(buf + 4);
    header.reserved = le16(buf + 6);
    header.web_len = le32(buf + 8);
    memcpy(header.web_sha256, buf + 12, sizeof(header.web_sha256));
    if (header.magic != OTA_BUNDLE_MAGIC || header.format_version != OTA_BUNDLE_FORMAT_VERSION) {
        return ESP_ERR_INVALID_VERSION;
    }
    *out = header;
    return ESP_OK;
}
