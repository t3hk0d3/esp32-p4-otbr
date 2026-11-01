#include <stdlib.h>
#include <string.h>
#include "unity.h"
#include "unity_test_runner.h"
#include "rcp_sync_proto.h"

#define TEST_BUF_LEN (RCP_SYNC_PROTO_HEADER_LEN + RCP_SYNC_PROTO_MAX_PAYLOAD + RCP_SYNC_PROTO_CRC_LEN)

TEST_CASE("On valid frame: decodes type, seq and payload identical to the encoded input", "[rcp_sync_proto]")
{
    const uint8_t payload[] = {1, 2, 3, 4, 5};
    const rcp_sync_proto_frame_t in = {.type = RCP_SYNC_MSG_DATA, .seq = 7, .len = sizeof(payload), .payload = payload};
    uint8_t buf[64];
    size_t n = 0;
    rcp_sync_proto_frame_t out = {0};

    TEST_ASSERT_EQUAL(ESP_OK, rcp_sync_proto_encode(&in, buf, sizeof(buf), &n));
    TEST_ASSERT_EQUAL(RCP_SYNC_PROTO_HEADER_LEN + sizeof(payload) + RCP_SYNC_PROTO_CRC_LEN, n);
    TEST_ASSERT_EQUAL_HEX8(0x53, buf[0]); /* magic 0x5253 little-endian */
    TEST_ASSERT_EQUAL_HEX8(0x52, buf[1]);
    TEST_ASSERT_EQUAL(ESP_OK, rcp_sync_proto_decode(buf, n, &out));
    TEST_ASSERT_EQUAL_UINT8(in.type, out.type);
    TEST_ASSERT_EQUAL_UINT8(in.seq, out.seq);
    TEST_ASSERT_EQUAL_UINT16(in.len, out.len);
    TEST_ASSERT_EQUAL_MEMORY(payload, out.payload, sizeof(payload));
}

TEST_CASE("On CRC mismatch: returns ESP_ERR_INVALID_CRC", "[rcp_sync_proto]")
{
    const uint8_t payload[] = {9, 8, 7};
    const rcp_sync_proto_frame_t in = {.type = RCP_SYNC_MSG_BEGIN, .seq = 1, .len = sizeof(payload), .payload = payload};
    uint8_t buf[64];
    size_t n = 0;
    rcp_sync_proto_frame_t out;

    TEST_ASSERT_EQUAL(ESP_OK, rcp_sync_proto_encode(&in, buf, sizeof(buf), &n));
    buf[RCP_SYNC_PROTO_HEADER_LEN] ^= 0x01;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_CRC, rcp_sync_proto_decode(buf, n, &out));
}

TEST_CASE("On payload length > 4096: encode returns ESP_ERR_INVALID_SIZE", "[rcp_sync_proto]")
{
    uint8_t *payload = calloc(1, RCP_SYNC_PROTO_MAX_PAYLOAD + 1);
    uint8_t *buf = calloc(1, TEST_BUF_LEN + 1);
    size_t n = 0;
    rcp_sync_proto_frame_t in = {.type = RCP_SYNC_MSG_DATA, .seq = 0, .len = RCP_SYNC_PROTO_MAX_PAYLOAD + 1, .payload = payload};

    TEST_ASSERT_NOT_NULL(payload);
    TEST_ASSERT_NOT_NULL(buf);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, rcp_sync_proto_encode(&in, buf, TEST_BUF_LEN + 1, &n));
    in.len = RCP_SYNC_PROTO_MAX_PAYLOAD;
    TEST_ASSERT_EQUAL(ESP_OK, rcp_sync_proto_encode(&in, buf, TEST_BUF_LEN, &n));
    free(payload);
    free(buf);
}

TEST_CASE("On buffer shorter than header + len + crc: decode returns ESP_ERR_INVALID_SIZE", "[rcp_sync_proto]")
{
    const uint8_t payload[] = {1, 2, 3, 4};
    const rcp_sync_proto_frame_t in = {.type = RCP_SYNC_MSG_OK, .seq = 2, .len = sizeof(payload), .payload = payload};
    uint8_t buf[64];
    size_t n = 0;
    rcp_sync_proto_frame_t out;

    TEST_ASSERT_EQUAL(ESP_OK, rcp_sync_proto_encode(&in, buf, sizeof(buf), &n));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, rcp_sync_proto_decode(buf, n - 1, &out));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, rcp_sync_proto_decode(buf, RCP_SYNC_PROTO_HEADER_LEN - 1, &out));
}

TEST_CASE("On wrong magic: decode returns ESP_ERR_INVALID_RESPONSE", "[rcp_sync_proto]")
{
    const rcp_sync_proto_frame_t in = {.type = RCP_SYNC_MSG_HELLO, .seq = 0, .len = 0, .payload = NULL};
    uint8_t buf[16];
    size_t n = 0;
    rcp_sync_proto_frame_t out;

    TEST_ASSERT_EQUAL(ESP_OK, rcp_sync_proto_encode(&in, buf, sizeof(buf), &n));
    buf[0] ^= 0xFF;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_RESPONSE, rcp_sync_proto_decode(buf, n, &out));
}

TEST_CASE("On round trip: all fields equal, version strings NUL-terminated", "[rcp_sync_proto]")
{
    rcp_sync_hello_reply_t in = {.proto_version = RCP_SYNC_PROTO_VERSION, .slot_size = 0x1A0000};
    rcp_sync_hello_reply_t out;
    uint8_t buf[RCP_SYNC_PROTO_HELLO_REPLY_LEN];
    size_t n = 0;

    strcpy(in.updater_version, "base-1a2b3c4d");
    strcpy(in.boot_slot_version, "20261029T090000Z-def5678");
    in.other_slot_version[0] = '\0';
    TEST_ASSERT_EQUAL(ESP_OK, rcp_sync_proto_put_hello_reply(&in, buf, sizeof(buf), &n));
    TEST_ASSERT_EQUAL(RCP_SYNC_PROTO_HELLO_REPLY_LEN, n);
    TEST_ASSERT_EQUAL(ESP_OK, rcp_sync_proto_get_hello_reply(buf, n, &out));
    TEST_ASSERT_EQUAL_UINT16(in.proto_version, out.proto_version);
    TEST_ASSERT_EQUAL_STRING(in.updater_version, out.updater_version);
    TEST_ASSERT_EQUAL_STRING(in.boot_slot_version, out.boot_slot_version);
    TEST_ASSERT_EQUAL_STRING("", out.other_slot_version);
    TEST_ASSERT_EQUAL_UINT32(in.slot_size, out.slot_size);
    TEST_ASSERT_EQUAL_UINT8(0, out.updater_version[RCP_SYNC_PROTO_VERSION_STR_LEN - 1]);
    TEST_ASSERT_EQUAL_UINT8(0, out.boot_slot_version[RCP_SYNC_PROTO_VERSION_STR_LEN - 1]);
    TEST_ASSERT_EQUAL_UINT8(0, out.other_slot_version[RCP_SYNC_PROTO_VERSION_STR_LEN - 1]);
}

TEST_CASE("On version string of 31 chars: preserved exactly", "[rcp_sync_proto]")
{
    rcp_sync_hello_reply_t in = {.proto_version = 1, .slot_size = 1};
    rcp_sync_hello_reply_t out;
    uint8_t buf[RCP_SYNC_PROTO_HELLO_REPLY_LEN];
    size_t n = 0;

    memset(in.boot_slot_version, 'v', 31);
    in.boot_slot_version[31] = '\0';
    TEST_ASSERT_EQUAL(ESP_OK, rcp_sync_proto_put_hello_reply(&in, buf, sizeof(buf), &n));
    TEST_ASSERT_EQUAL(ESP_OK, rcp_sync_proto_get_hello_reply(buf, n, &out));
    TEST_ASSERT_EQUAL(31, strlen(out.boot_slot_version));
    TEST_ASSERT_EQUAL_STRING(in.boot_slot_version, out.boot_slot_version);
}

void app_main(void)
{
    UNITY_BEGIN();
    unity_run_all_tests();
    int failures = (int)UNITY_END();
    exit(failures == 0 ? 0 : 1);
}
