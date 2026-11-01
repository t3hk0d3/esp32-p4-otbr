#include <stdlib.h>
#include <string.h>
#include "unity.h"
#include "unity_test_runner.h"
#include "updater_core.h"

#define FAKE_SLOT_SIZE 1024
#define UPDATER_VER    "base-1a2b3c4d"
#define BOOT_VER       "20261029T080000Z-1234567"
#define REPLY_CAP      (RCP_SYNC_PROTO_HEADER_LEN + RCP_SYNC_PROTO_HELLO_REPLY_LEN + RCP_SYNC_PROTO_CRC_LEN)

/* Fake flash: one in-memory inactive slot, configurable versions and verify result, call counters. */
static struct {
    bool boot_has_image;
    bool other_has_image;
    bool verify_ok;
    bool boot_switched;
    bool fail_begin;
    bool fail_write;
    int begin_calls;
    int write_calls;
    int finish_calls;
    uint8_t slot[FAKE_SLOT_SIZE];
} s_fake;

static esp_err_t fake_slot_version(bool boot_slot, char *out, size_t len)
{
    if (!(boot_slot ? s_fake.boot_has_image : s_fake.other_has_image)) {
        return ESP_ERR_NOT_FOUND;
    }
    strlcpy(out, boot_slot ? BOOT_VER : "20261028T000000Z-0000000", len);
    return ESP_OK;
}

static esp_err_t fake_begin(uint32_t image_size)
{
    (void)image_size;
    s_fake.begin_calls++;
    if (s_fake.fail_begin) {
        return ESP_FAIL;
    }
    memset(s_fake.slot, 0xff, sizeof(s_fake.slot));
    return ESP_OK;
}

static esp_err_t fake_write(uint32_t offset, const void *data, size_t len)
{
    s_fake.write_calls++;
    if (s_fake.fail_write) {
        return ESP_FAIL;
    }
    memcpy(&s_fake.slot[offset], data, len);
    return ESP_OK;
}

static esp_err_t fake_finish(void)
{
    s_fake.finish_calls++;
    if (!s_fake.verify_ok) {
        return ESP_FAIL;
    }
    s_fake.boot_switched = true;
    return ESP_OK;
}

static const updater_flash_t s_flash = {
    .slot_version = fake_slot_version,
    .begin = fake_begin,
    .write = fake_write,
    .finish = fake_finish,
    .slot_size = FAKE_SLOT_SIZE,
};

static updater_core_t s_ctx;
static uint8_t s_reply[REPLY_CAP];
static rcp_sync_proto_frame_t s_frame; /* last decoded reply */

static void setup(void)
{
    memset(&s_fake, 0, sizeof(s_fake));
    s_fake.boot_has_image = true;
    s_fake.verify_ok = true;
    updater_core_init(&s_ctx, &s_flash, UPDATER_VER);
}

static esp_err_t send_raw(const uint8_t *req, size_t req_len)
{
    size_t n = 0;
    esp_err_t err = updater_core_handle(&s_ctx, req, req_len, s_reply, sizeof(s_reply), &n);
    if (err == ESP_OK) {
        TEST_ASSERT_EQUAL(ESP_OK, rcp_sync_proto_decode(s_reply, n, &s_frame));
    }
    return err;
}

static void send(uint8_t type, uint8_t seq, const uint8_t *payload, uint16_t len)
{
    const rcp_sync_proto_frame_t f = {.type = type, .seq = seq, .len = len, .payload = payload};
    uint8_t req[RCP_SYNC_PROTO_HEADER_LEN + 8 + 64 + RCP_SYNC_PROTO_CRC_LEN];
    size_t n = 0;

    TEST_ASSERT_EQUAL(ESP_OK, rcp_sync_proto_encode(&f, req, sizeof(req), &n));
    TEST_ASSERT_EQUAL(ESP_OK, send_raw(req, n));
}

static void put_u32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++) {
        p[i] = (uint8_t)(v >> (8 * i));
    }
}

static uint32_t get_u32(const uint8_t *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void send_begin(uint8_t seq, uint32_t image_size)
{
    uint8_t p[4];
    put_u32(p, image_size);
    send(RCP_SYNC_MSG_BEGIN, seq, p, sizeof(p));
}

static void send_data(uint8_t seq, uint32_t offset, const uint8_t *bytes, size_t n)
{
    uint8_t p[4 + 16];
    put_u32(p, offset);
    memcpy(&p[4], bytes, n);
    send(RCP_SYNC_MSG_DATA, seq, p, (uint16_t)(4 + n));
}

static void expect_err(uint16_t code, uint32_t expected_offset)
{
    TEST_ASSERT_EQUAL(RCP_SYNC_MSG_ERR, s_frame.type);
    TEST_ASSERT_EQUAL_UINT16(6, s_frame.len);
    TEST_ASSERT_EQUAL_UINT16(code, s_frame.payload[0] | (s_frame.payload[1] << 8));
    TEST_ASSERT_EQUAL_UINT32(expected_offset, get_u32(&s_frame.payload[2]));
}

static rcp_sync_hello_reply_t hello(void)
{
    rcp_sync_hello_reply_t h;

    send(RCP_SYNC_MSG_HELLO, 1, NULL, 0);
    TEST_ASSERT_EQUAL(RCP_SYNC_MSG_OK, s_frame.type);
    TEST_ASSERT_EQUAL(ESP_OK, rcp_sync_proto_get_hello_reply(s_frame.payload, s_frame.len, &h));
    return h;
}

/* Brings the core to "BEGIN accepted, 4 bytes of 8 written" (seq 1..2). */
static void begin_with_half_written(void)
{
    const uint8_t half[4] = {1, 2, 3, 4};

    send_begin(1, 8);
    send_data(2, 0, half, sizeof(half));
}

TEST_CASE("On boot slot with verifiable image: replies boot_slot_version equal to that image's version", "[updater_core]")
{
    setup();
    TEST_ASSERT_EQUAL_STRING(BOOT_VER, hello().boot_slot_version);
}

TEST_CASE("On boot slot empty or unverifiable: replies empty boot_slot_version", "[updater_core]")
{
    setup();
    s_fake.boot_has_image = false;
    TEST_ASSERT_EQUAL_STRING("", hello().boot_slot_version);
}

TEST_CASE("On any state: replies proto_version 1, updater_version, slot_size", "[updater_core]")
{
    setup();
    begin_with_half_written();
    rcp_sync_hello_reply_t h = hello();
    TEST_ASSERT_EQUAL_UINT16(1, h.proto_version);
    TEST_ASSERT_EQUAL_STRING(UPDATER_VER, h.updater_version);
    TEST_ASSERT_EQUAL_UINT32(FAKE_SLOT_SIZE, h.slot_size);
}

TEST_CASE("On image_size within slot size: calls begin (erases the inactive slot)", "[updater_core]")
{
    setup();
    send_begin(1, FAKE_SLOT_SIZE);
    TEST_ASSERT_EQUAL(1, s_fake.begin_calls);
}

TEST_CASE("On image_size within slot size: replies OK", "[updater_core]")
{
    setup();
    send_begin(1, 100);
    TEST_ASSERT_EQUAL(RCP_SYNC_MSG_OK, s_frame.type);
    TEST_ASSERT_EQUAL_UINT8(1, s_frame.seq);
}

TEST_CASE("On image_size larger than slot size: replies ERR(RCP_SYNC_ERR_SIZE) and does not erase", "[updater_core]")
{
    setup();
    send_begin(1, FAKE_SLOT_SIZE + 1);
    expect_err(RCP_SYNC_ERR_SIZE, 0);
    TEST_ASSERT_EQUAL(0, s_fake.begin_calls);
}

TEST_CASE("On offset equal to next expected offset: writes bytes to the inactive slot", "[updater_core]")
{
    const uint8_t first[4] = {1, 2, 3, 4};
    const uint8_t second[4] = {5, 6, 7, 8};

    setup();
    send_begin(1, 8);
    send_data(2, 0, first, sizeof(first));
    send_data(3, 4, second, sizeof(second));
    TEST_ASSERT_EQUAL_MEMORY("\x01\x02\x03\x04\x05\x06\x07\x08", s_fake.slot, 8);
}

TEST_CASE("On offset equal to next expected offset: replies OK(next_offset)", "[updater_core]")
{
    const uint8_t first[4] = {1, 2, 3, 4};

    setup();
    send_begin(1, 8);
    send_data(2, 0, first, sizeof(first));
    TEST_ASSERT_EQUAL(RCP_SYNC_MSG_OK, s_frame.type);
    TEST_ASSERT_EQUAL_UINT16(4, s_frame.len);
    TEST_ASSERT_EQUAL_UINT32(4, get_u32(s_frame.payload));
}

TEST_CASE("On unexpected offset: replies ERR(RCP_SYNC_ERR_OFFSET, expected_offset) and writes nothing", "[updater_core]")
{
    const uint8_t bytes[2] = {9, 9};

    setup();
    begin_with_half_written();
    int writes = s_fake.write_calls;
    send_data(3, 6, bytes, sizeof(bytes));
    expect_err(RCP_SYNC_ERR_OFFSET, 4);
    TEST_ASSERT_EQUAL(writes, s_fake.write_calls);
}

TEST_CASE("On DATA before BEGIN: replies ERR(RCP_SYNC_ERR_STATE)", "[updater_core]")
{
    const uint8_t bytes[2] = {9, 9};

    setup();
    send_data(1, 0, bytes, sizeof(bytes));
    expect_err(RCP_SYNC_ERR_STATE, 0);
}

TEST_CASE("On repeated seq: replies the cached reply without writing", "[updater_core]")
{
    const uint8_t bytes[4] = {1, 2, 3, 4};
    uint8_t first_reply[REPLY_CAP];

    setup();
    send_begin(1, 8);
    send_data(2, 0, bytes, sizeof(bytes));
    memcpy(first_reply, s_reply, sizeof(first_reply));
    int writes = s_fake.write_calls;

    send_data(2, 0, bytes, sizeof(bytes));
    TEST_ASSERT_EQUAL(writes, s_fake.write_calls);
    TEST_ASSERT_EQUAL_MEMORY(first_reply, s_reply, RCP_SYNC_PROTO_HEADER_LEN + s_frame.len + RCP_SYNC_PROTO_CRC_LEN);
}

TEST_CASE("On image that verifies: calls finish (boot slot switched)", "[updater_core]")
{
    const uint8_t bytes[4] = {1, 2, 3, 4};

    setup();
    send_begin(1, 4);
    send_data(2, 0, bytes, sizeof(bytes));
    send(RCP_SYNC_MSG_END, 3, NULL, 0);
    TEST_ASSERT_EQUAL(1, s_fake.finish_calls);
    TEST_ASSERT_TRUE(s_fake.boot_switched);
}

TEST_CASE("On image that verifies: replies OK", "[updater_core]")
{
    const uint8_t bytes[4] = {1, 2, 3, 4};

    setup();
    send_begin(1, 4);
    send_data(2, 0, bytes, sizeof(bytes));
    send(RCP_SYNC_MSG_END, 3, NULL, 0);
    TEST_ASSERT_EQUAL(RCP_SYNC_MSG_OK, s_frame.type);
}

TEST_CASE("On image that fails verification: leaves boot slot unchanged", "[updater_core]")
{
    const uint8_t bytes[4] = {1, 2, 3, 4};

    setup();
    s_fake.verify_ok = false;
    send_begin(1, 4);
    send_data(2, 0, bytes, sizeof(bytes));
    send(RCP_SYNC_MSG_END, 3, NULL, 0);
    TEST_ASSERT_EQUAL(1, s_fake.finish_calls);
    TEST_ASSERT_FALSE(s_fake.boot_switched);
}

TEST_CASE("On image that fails verification: replies ERR(RCP_SYNC_ERR_VERIFY)", "[updater_core]")
{
    const uint8_t bytes[4] = {1, 2, 3, 4};

    setup();
    s_fake.verify_ok = false;
    send_begin(1, 4);
    send_data(2, 0, bytes, sizeof(bytes));
    send(RCP_SYNC_MSG_END, 3, NULL, 0);
    expect_err(RCP_SYNC_ERR_VERIFY, 0);
}

TEST_CASE("On END with fewer bytes than image_size: replies ERR(RCP_SYNC_ERR_STATE)", "[updater_core]")
{
    setup();
    begin_with_half_written();
    send(RCP_SYNC_MSG_END, 3, NULL, 0);
    expect_err(RCP_SYNC_ERR_STATE, 4);
    TEST_ASSERT_EQUAL(0, s_fake.finish_calls);
}

TEST_CASE("On CRC mismatch: replies NAK", "[updater_core]")
{
    const rcp_sync_proto_frame_t f = {.type = RCP_SYNC_MSG_HELLO, .seq = 1};
    uint8_t req[RCP_SYNC_PROTO_HEADER_LEN + RCP_SYNC_PROTO_CRC_LEN];
    size_t n = 0;

    setup();
    TEST_ASSERT_EQUAL(ESP_OK, rcp_sync_proto_encode(&f, req, sizeof(req), &n));
    req[n - 1] ^= 0xff;
    TEST_ASSERT_EQUAL(ESP_OK, send_raw(req, n));
    TEST_ASSERT_EQUAL(RCP_SYNC_MSG_NAK, s_frame.type);
}

TEST_CASE("On same seq with a different request: executes it instead of replaying", "[updater_core]")
{
    setup();
    send_begin(1, 8);
    send_begin(1, 16);
    TEST_ASSERT_EQUAL(2, s_fake.begin_calls);
    TEST_ASSERT_EQUAL(RCP_SYNC_MSG_OK, s_frame.type);
}

TEST_CASE("On HELLO repeating the previous seq: replies fresh versions", "[updater_core]")
{
    setup();
    TEST_ASSERT_EQUAL_STRING(BOOT_VER, hello().boot_slot_version);
    s_fake.boot_has_image = false;
    TEST_ASSERT_EQUAL_STRING("", hello().boot_slot_version);
}

TEST_CASE("On BEGIN twice: both succeed and the second restarts the image", "[updater_core]")
{
    const uint8_t bytes[4] = {1, 2, 3, 4};

    setup();
    begin_with_half_written();
    send_begin(3, 8);
    TEST_ASSERT_EQUAL(RCP_SYNC_MSG_OK, s_frame.type);
    TEST_ASSERT_EQUAL(2, s_fake.begin_calls);
    send_data(4, 0, bytes, sizeof(bytes));
    TEST_ASSERT_EQUAL(RCP_SYNC_MSG_OK, s_frame.type);
    TEST_ASSERT_EQUAL_UINT32(4, get_u32(s_frame.payload));
}

TEST_CASE("On flash begin error: replies ERR(RCP_SYNC_ERR_FLASH) and DATA is then rejected", "[updater_core]")
{
    const uint8_t bytes[2] = {9, 9};

    setup();
    s_fake.fail_begin = true;
    send_begin(1, 8);
    expect_err(RCP_SYNC_ERR_FLASH, 0);
    send_data(2, 0, bytes, sizeof(bytes));
    expect_err(RCP_SYNC_ERR_STATE, 0);
}

TEST_CASE("On flash write error: replies ERR(RCP_SYNC_ERR_FLASH, expected_offset) and offset does not advance", "[updater_core]")
{
    const uint8_t bytes[4] = {1, 2, 3, 4};

    setup();
    send_begin(1, 8);
    s_fake.fail_write = true;
    send_data(2, 0, bytes, sizeof(bytes));
    expect_err(RCP_SYNC_ERR_FLASH, 0);
    s_fake.fail_write = false;
    send_data(3, 0, bytes, sizeof(bytes));
    TEST_ASSERT_EQUAL(RCP_SYNC_MSG_OK, s_frame.type);
}

TEST_CASE("On truncated frame: replies NAK", "[updater_core]")
{
    const rcp_sync_proto_frame_t f = {.type = RCP_SYNC_MSG_HELLO, .seq = 1};
    uint8_t req[RCP_SYNC_PROTO_HEADER_LEN + RCP_SYNC_PROTO_CRC_LEN];
    size_t n = 0;

    setup();
    TEST_ASSERT_EQUAL(ESP_OK, rcp_sync_proto_encode(&f, req, sizeof(req), &n));
    TEST_ASSERT_EQUAL(ESP_OK, send_raw(req, n - 2));
    TEST_ASSERT_EQUAL(RCP_SYNC_MSG_NAK, s_frame.type);
    TEST_ASSERT_EQUAL(ESP_OK, send_raw(req, 0));
    TEST_ASSERT_EQUAL(RCP_SYNC_MSG_NAK, s_frame.type);
}

TEST_CASE("On wrong magic: replies NAK", "[updater_core]")
{
    const rcp_sync_proto_frame_t f = {.type = RCP_SYNC_MSG_HELLO, .seq = 1};
    uint8_t req[RCP_SYNC_PROTO_HEADER_LEN + RCP_SYNC_PROTO_CRC_LEN];
    size_t n = 0;

    setup();
    TEST_ASSERT_EQUAL(ESP_OK, rcp_sync_proto_encode(&f, req, sizeof(req), &n));
    req[0] ^= 0xff;
    TEST_ASSERT_EQUAL(ESP_OK, send_raw(req, n));
    TEST_ASSERT_EQUAL(RCP_SYNC_MSG_NAK, s_frame.type);
}

void app_main(void)
{
    UNITY_BEGIN();
    unity_run_all_tests();
    int failures = (int)UNITY_END();
    exit(failures == 0 ? 0 : 1);
}

TEST_CASE("Blank otadata, only ota_1 valid: the boot slot is ota_1 (as the bootloader falls forward)", "[updater_core]")
{
    TEST_ASSERT_EQUAL_INT(1, updater_core_choose_boot_slot(UPDATER_OTADATA_NONE, false, true));
}

TEST_CASE("Blank otadata, only ota_1 valid: the next push goes to ota_0", "[updater_core]")
{
    int boot = updater_core_choose_boot_slot(UPDATER_OTADATA_NONE, false, true);

    TEST_ASSERT_EQUAL_INT(0, 1 - boot);
}

TEST_CASE("Blank otadata, both slots valid: the boot slot is ota_0 (the first valid one)", "[updater_core]")
{
    TEST_ASSERT_EQUAL_INT(0, updater_core_choose_boot_slot(UPDATER_OTADATA_NONE, true, true));
}

TEST_CASE("Blank otadata, only ota_0 valid: the boot slot is ota_0", "[updater_core]")
{
    TEST_ASSERT_EQUAL_INT(0, updater_core_choose_boot_slot(UPDATER_OTADATA_NONE, true, false));
}

TEST_CASE("Blank otadata, no valid slot: the boot slot is ota_0 and the next push goes to ota_1", "[updater_core]")
{
    TEST_ASSERT_EQUAL_INT(0, updater_core_choose_boot_slot(UPDATER_OTADATA_NONE, false, false));
}

TEST_CASE("Otadata selects ota_1 and both slots are valid: the boot slot is ota_1", "[updater_core]")
{
    TEST_ASSERT_EQUAL_INT(1, updater_core_choose_boot_slot(UPDATER_OTADATA_SLOT_1, true, true));
}

TEST_CASE("Otadata selects ota_0 and both slots are valid: the boot slot is ota_0", "[updater_core]")
{
    TEST_ASSERT_EQUAL_INT(0, updater_core_choose_boot_slot(UPDATER_OTADATA_SLOT_0, true, true));
}

TEST_CASE("Otadata selects ota_1 but only ota_0 is valid: the boot slot is ota_0 (bootloader fallback)", "[updater_core]")
{
    TEST_ASSERT_EQUAL_INT(0, updater_core_choose_boot_slot(UPDATER_OTADATA_SLOT_1, true, false));
}

TEST_CASE("Otadata selects ota_0 but only ota_1 is valid: the boot slot is ota_1 (bootloader fallback)", "[updater_core]")
{
    TEST_ASSERT_EQUAL_INT(1, updater_core_choose_boot_slot(UPDATER_OTADATA_SLOT_0, false, true));
}

TEST_CASE("Otadata selects ota_1 and no slot is valid: the boot slot stays ota_1", "[updater_core]")
{
    TEST_ASSERT_EQUAL_INT(1, updater_core_choose_boot_slot(UPDATER_OTADATA_SLOT_1, false, false));
}
