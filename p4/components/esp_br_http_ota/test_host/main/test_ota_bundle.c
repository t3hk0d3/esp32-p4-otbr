#include <string.h>
#include "unity.h"
#include "ota_bundle.h"

static void make_header(uint8_t buf[OTA_BUNDLE_HEADER_LEN])
{
    memset(buf, 0, OTA_BUNDLE_HEADER_LEN);
    memcpy(buf, "OTBB", 4); /* magic 0x4242544F, little-endian */
    buf[4] = 1;             /* format_version 1 */
    buf[8] = 0x78;          /* web_len 0x00123478 */
    buf[9] = 0x34;
    buf[10] = 0x12;
    for (int i = 0; i < 32; i++) {
        buf[12 + i] = (uint8_t)(0xA0 + i);
    }
}

TEST_CASE("On valid header: parses magic, format_version, web_len, web_sha256", "[ota_bundle]")
{
    uint8_t buf[OTA_BUNDLE_HEADER_LEN + 5];
    make_header(buf);
    ota_bundle_header_t header;
    TEST_ASSERT_EQUAL(ESP_OK, ota_bundle_parse_header(buf, sizeof(buf), &header));
    TEST_ASSERT_EQUAL_HEX32(0x4242544F, header.magic);
    TEST_ASSERT_EQUAL_UINT16(1, header.format_version);
    TEST_ASSERT_EQUAL_UINT32(0x00123478, header.web_len);
    TEST_ASSERT_EQUAL_MEMORY(buf + 12, header.web_sha256, 32);
}

TEST_CASE("On wrong magic or format_version: returns ESP_ERR_INVALID_VERSION", "[ota_bundle]")
{
    uint8_t buf[OTA_BUNDLE_HEADER_LEN];
    ota_bundle_header_t header;
    make_header(buf);
    buf[0] = 'X';
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_VERSION, ota_bundle_parse_header(buf, sizeof(buf), &header));
    make_header(buf);
    buf[4] = 2;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_VERSION, ota_bundle_parse_header(buf, sizeof(buf), &header));
}

TEST_CASE("On buffer shorter than 44 bytes: returns ESP_ERR_INVALID_SIZE", "[ota_bundle]")
{
    uint8_t buf[OTA_BUNDLE_HEADER_LEN];
    ota_bundle_header_t header;
    make_header(buf);
    for (size_t len = 0; len < OTA_BUNDLE_HEADER_LEN; len++) {
        TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, ota_bundle_parse_header(buf, len, &header));
    }
}
