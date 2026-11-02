#include <stdlib.h>
#include <string.h>
#include "unity.h"
#include "unity_test_runner.h"
#include "c6_board.h"
#include "c6_bundle.h"

static c6_bundle_t sample(void)
{
    c6_bundle_t b = {0};
    const uint32_t lengths[C6_BUNDLE_ENTRIES] = {22464, 3072, 228336, 277824};

    for (int i = 0; i < C6_BUNDLE_ENTRIES; i++) {
        b.entries[i].kind = (uint32_t)(i + 1);
        b.entries[i].length = lengths[i];
        memset(b.entries[i].sha256, 0x11 * (i + 1), C6_BUNDLE_SHA256_LEN);
    }
    return b;
}

TEST_CASE("On four entries round-tripped: decode returns the same kinds, lengths and hashes", "[c6_bundle]")
{
    const c6_bundle_t in = sample();
    uint8_t buf[C6_BUNDLE_HEADER_LEN];
    c6_bundle_t out;

    TEST_ASSERT_EQUAL(ESP_OK, c6_bundle_header_encode(&in, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_MEMORY(C6_BUNDLE_MAGIC, buf, C6_BUNDLE_MAGIC_LEN);
    TEST_ASSERT_EQUAL(ESP_OK, c6_bundle_header_decode(buf, sizeof(buf), &out));
    TEST_ASSERT_EQUAL_MEMORY(&in, &out, sizeof(in));
    TEST_ASSERT_EQUAL_UINT32(22464 + 3072 + 228336 + 277824, c6_bundle_data_len(&out));
}

TEST_CASE("On a wrong magic: ESP_ERR_NOT_SUPPORTED, out untouched", "[c6_bundle]")
{
    const c6_bundle_t in = sample();
    uint8_t buf[C6_BUNDLE_HEADER_LEN];
    c6_bundle_t out;

    memset(&out, 0xAB, sizeof(out));
    TEST_ASSERT_EQUAL(ESP_OK, c6_bundle_header_encode(&in, buf, sizeof(buf)));
    buf[0] ^= 0xFF;
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_SUPPORTED, c6_bundle_header_decode(buf, sizeof(buf), &out));
    TEST_ASSERT_EQUAL_HEX8(0xAB, ((uint8_t *)&out)[0]);
}

TEST_CASE("On a wrong format version: ESP_ERR_NOT_SUPPORTED", "[c6_bundle]")
{
    const c6_bundle_t in = sample();
    uint8_t buf[C6_BUNDLE_HEADER_LEN];
    c6_bundle_t out;

    TEST_ASSERT_EQUAL(ESP_OK, c6_bundle_header_encode(&in, buf, sizeof(buf)));
    buf[C6_BUNDLE_MAGIC_LEN] = 2;
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_SUPPORTED, c6_bundle_header_decode(buf, sizeof(buf), &out));
}

TEST_CASE("On a header byte changed after encoding: ESP_ERR_INVALID_CRC", "[c6_bundle]")
{
    const c6_bundle_t in = sample();
    uint8_t buf[C6_BUNDLE_HEADER_LEN];
    c6_bundle_t out;

    TEST_ASSERT_EQUAL(ESP_OK, c6_bundle_header_encode(&in, buf, sizeof(buf)));
    buf[C6_BUNDLE_MAGIC_LEN + 8 + 40 + 9] ^= 0x01; /* a hash byte of the second entry */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_CRC, c6_bundle_header_decode(buf, sizeof(buf), &out));
}

TEST_CASE("On a short buffer: ESP_ERR_INVALID_SIZE", "[c6_bundle]")
{
    const c6_bundle_t in = sample();
    uint8_t buf[C6_BUNDLE_HEADER_LEN];
    c6_bundle_t out;

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, c6_bundle_header_encode(&in, buf, sizeof(buf) - 1));
    TEST_ASSERT_EQUAL(ESP_OK, c6_bundle_header_encode(&in, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, c6_bundle_header_decode(buf, sizeof(buf) - 1, &out));
}

TEST_CASE("On a blob larger than its room: encode refuses with ESP_ERR_INVALID_SIZE", "[c6_bundle]")
{
    const uint32_t too_big[C6_BUNDLE_ENTRIES] = {
        C6_FLASH_PTABLE_OFFSET + 1,                    /* bootloader runs into the partition table */
        C6_FLASH_NVS_OFFSET - C6_FLASH_PTABLE_OFFSET + 1, /* partition table runs into nvs */
        C6_FLASH_TEST_SIZE + 1,                        /* updater larger than test */
        C6_BUNDLE_TARGET_END - C6_FLASH_OTA0_OFFSET + 1, /* RCP runs into stock ota_1 */
    };
    uint8_t buf[C6_BUNDLE_HEADER_LEN];

    for (int i = 0; i < C6_BUNDLE_ENTRIES; i++) {
        c6_bundle_t b = sample();
        b.entries[i].length = too_big[i];
        TEST_ASSERT_EQUAL_MESSAGE(ESP_ERR_INVALID_SIZE, c6_bundle_header_encode(&b, buf, sizeof(buf)), "entry");
    }
}

TEST_CASE("On an empty blob or kinds out of order: encode refuses with ESP_ERR_INVALID_SIZE", "[c6_bundle]")
{
    uint8_t buf[C6_BUNDLE_HEADER_LEN];
    c6_bundle_t b = sample();

    b.entries[2].length = 0;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, c6_bundle_header_encode(&b, buf, sizeof(buf)));
    b = sample();
    b.entries[0].kind = C6_BUNDLE_RCP;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, c6_bundle_header_encode(&b, buf, sizeof(buf)));
}

TEST_CASE("On the largest blobs that fit: encode and decode accept them", "[c6_bundle]")
{
    c6_bundle_t b = sample();
    uint8_t buf[C6_BUNDLE_HEADER_LEN];
    c6_bundle_t out;

    b.entries[0].length = C6_FLASH_PTABLE_OFFSET;
    b.entries[1].length = C6_FLASH_NVS_OFFSET - C6_FLASH_PTABLE_OFFSET;
    b.entries[2].length = C6_FLASH_TEST_SIZE;
    b.entries[3].length = C6_BUNDLE_TARGET_END - C6_FLASH_OTA0_OFFSET;
    TEST_ASSERT_EQUAL(ESP_OK, c6_bundle_header_encode(&b, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL(ESP_OK, c6_bundle_header_decode(buf, sizeof(buf), &out));
}

TEST_CASE("On target lookups: offsets follow c6_board.h", "[c6_bundle]")
{
    TEST_ASSERT_EQUAL_HEX32(C6_FLASH_BOOTLOADER_OFFSET, c6_bundle_target_offset(C6_BUNDLE_BOOTLOADER));
    TEST_ASSERT_EQUAL_HEX32(C6_FLASH_PTABLE_OFFSET, c6_bundle_target_offset(C6_BUNDLE_PTABLE));
    TEST_ASSERT_EQUAL_HEX32(C6_FLASH_TEST_OFFSET, c6_bundle_target_offset(C6_BUNDLE_UPDATER));
    TEST_ASSERT_EQUAL_HEX32(C6_FLASH_OTA0_OFFSET, c6_bundle_target_offset(C6_BUNDLE_RCP));
    TEST_ASSERT_EQUAL_HEX32(0, c6_bundle_target_room((c6_bundle_kind_t)9));
}

TEST_CASE("On header offsets: the next 4 KiB boundary after the image", "[c6_bundle]")
{
    TEST_ASSERT_EQUAL_HEX32(0x1000, c6_bundle_header_offset(1));
    TEST_ASSERT_EQUAL_HEX32(0x1000, c6_bundle_header_offset(0x1000));
    TEST_ASSERT_EQUAL_HEX32(0x26000, c6_bundle_header_offset(0x25001));
}

TEST_CASE("On SHA-256 of \"abc\": matches the FIPS 180-2 vector, one-shot and in chunks", "[c6_bundle]")
{
    static const uint8_t expected[C6_BUNDLE_SHA256_LEN] = {
        0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
        0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad,
    };
    uint8_t got[C6_BUNDLE_SHA256_LEN];
    c6_bundle_sha256_t ctx;

    TEST_ASSERT_EQUAL(ESP_OK, c6_bundle_sha256("abc", 3, got));
    TEST_ASSERT_EQUAL_MEMORY(expected, got, sizeof(got));
    memset(got, 0, sizeof(got));
    TEST_ASSERT_EQUAL(ESP_OK, c6_bundle_sha256_begin(&ctx));
    TEST_ASSERT_EQUAL(ESP_OK, c6_bundle_sha256_update(&ctx, "a", 1));
    TEST_ASSERT_EQUAL(ESP_OK, c6_bundle_sha256_update(&ctx, "bc", 2));
    TEST_ASSERT_EQUAL(ESP_OK, c6_bundle_sha256_finish(&ctx, got));
    TEST_ASSERT_EQUAL_MEMORY(expected, got, sizeof(got));
}

void app_main(void)
{
    UNITY_BEGIN();
    unity_run_all_tests();
    int failures = (int)UNITY_END();
    exit(failures == 0 ? 0 : 1);
}
