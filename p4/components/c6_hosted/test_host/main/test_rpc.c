#include <string.h>
#include "unity.h"
#include "hosted_rpc.h"

/* ---- hosted_rpc_encode_req ---- */

TEST_CASE("On OTABegin: msg_type 1, msg_id 272, uid and an empty oneof field 272", "[rpc]")
{
    const uint8_t want[] = {0x08, 0x01, 0x10, 0x90, 0x02, 0x18, 0x07, 0x82, 0x11, 0x00};
    uint8_t out[32];

    TEST_ASSERT_EQUAL(sizeof(want), hosted_rpc_encode_req(HOSTED_RPC_REQ_OTA_BEGIN, 7, NULL, 0, out, sizeof(out)));
    TEST_ASSERT_EQUAL_MEMORY(want, out, sizeof(want));
}

TEST_CASE("On OTAEnd: msg_id 274 and an empty oneof field 274", "[rpc]")
{
    const uint8_t want[] = {0x08, 0x01, 0x10, 0x92, 0x02, 0x18, 0x01, 0x92, 0x11, 0x00};
    uint8_t out[32];

    TEST_ASSERT_EQUAL(sizeof(want), hosted_rpc_encode_req(HOSTED_RPC_REQ_OTA_END, 1, NULL, 0, out, sizeof(out)));
    TEST_ASSERT_EQUAL_MEMORY(want, out, sizeof(want));
}

TEST_CASE("On OTAWrite of 1400 bytes: field 273 holds Req_OTAWrite{ota_data = 1} with the bytes", "[rpc]")
{
    static uint8_t data[HOSTED_RPC_OTA_CHUNK];
    static uint8_t out[HOSTED_RPC_REQ_MAX];
    for (size_t i = 0; i < sizeof(data); i++) {
        data[i] = (uint8_t)i;
    }
    const uint8_t head[] = {0x08, 0x01, 0x10, 0x91, 0x02, 0x18, 0x02, 0x8A, 0x11, 0xFB, 0x0A, 0x0A, 0xF8, 0x0A};

    const size_t n = hosted_rpc_encode_req(HOSTED_RPC_REQ_OTA_WRITE, 2, data, sizeof(data), out, sizeof(out));
    TEST_ASSERT_EQUAL(sizeof(head) + sizeof(data), n);
    TEST_ASSERT_EQUAL_MEMORY(head, out, sizeof(head));
    TEST_ASSERT_EQUAL_MEMORY(data, out + sizeof(head), sizeof(data));
}

TEST_CASE("On another msg id, data on OTABegin or too small a buffer: returns 0", "[rpc]")
{
    uint8_t out[32];
    const uint8_t data[4] = {0};

    TEST_ASSERT_EQUAL(0, hosted_rpc_encode_req(300, 1, NULL, 0, out, sizeof(out)));
    TEST_ASSERT_EQUAL(0, hosted_rpc_encode_req(HOSTED_RPC_REQ_OTA_BEGIN, 1, data, sizeof(data), out, sizeof(out)));
    TEST_ASSERT_EQUAL(0, hosted_rpc_encode_req(HOSTED_RPC_REQ_OTA_BEGIN, 1, NULL, 0, out, 5));
}

/* ---- hosted_rpc_decode ---- */

TEST_CASE("On Resp_OTABegin with resp 0: msg_type 2, msg_id 528, uid, resp 0", "[rpc]")
{
    const uint8_t in[] = {0x08, 0x02, 0x10, 0x90, 0x04, 0x18, 0x05, 0x82, 0x21, 0x02, 0x08, 0x00};
    hosted_rpc_msg_t m;

    TEST_ASSERT_EQUAL(ESP_OK, hosted_rpc_decode(in, sizeof(in), &m));
    TEST_ASSERT_EQUAL(HOSTED_RPC_TYPE_RESP, m.msg_type);
    TEST_ASSERT_EQUAL(HOSTED_RPC_REQ_OTA_BEGIN + HOSTED_RPC_RESP_OFFSET, m.msg_id);
    TEST_ASSERT_EQUAL(5, m.uid);
    TEST_ASSERT_EQUAL(0, m.resp);
}

TEST_CASE("On a response with resp -1 (10-byte varint): resp is -1", "[rpc]")
{
    const uint8_t in[] = {0x08, 0x02, 0x10, 0x91, 0x04, 0x8A, 0x21, 0x0B, 0x08,
                          0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01
                         };
    hosted_rpc_msg_t m;

    TEST_ASSERT_EQUAL(ESP_OK, hosted_rpc_decode(in, sizeof(in), &m));
    TEST_ASSERT_EQUAL(HOSTED_RPC_REQ_OTA_WRITE + HOSTED_RPC_RESP_OFFSET, m.msg_id);
    TEST_ASSERT_EQUAL(-1, m.resp);
}

TEST_CASE("On an event or unknown fields: they are skipped, resp stays 0", "[rpc]")
{
    /* Event ESPInit (769) with a payload in field 769, plus an unknown varint field 99 and a fixed32 field 100. */
    const uint8_t in[] = {0x08, 0x03, 0x10, 0x81, 0x06, 0x8A, 0x30, 0x02, 0x08, 0x05, 0x98, 0x06, 0x01, 0xA5, 0x06, 1, 2, 3, 4};
    hosted_rpc_msg_t m;

    TEST_ASSERT_EQUAL(ESP_OK, hosted_rpc_decode(in, sizeof(in), &m));
    TEST_ASSERT_EQUAL(HOSTED_RPC_TYPE_EVENT, m.msg_type);
    TEST_ASSERT_EQUAL(769, m.msg_id);
    TEST_ASSERT_EQUAL(0, m.resp);
}

TEST_CASE("On truncated protobuf: ESP_ERR_INVALID_RESPONSE", "[rpc]")
{
    const uint8_t in[] = {0x08, 0x02, 0x10, 0x90, 0x04, 0x82, 0x21, 0x05, 0x08};
    hosted_rpc_msg_t m;

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_RESPONSE, hosted_rpc_decode(in, sizeof(in), &m));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_RESPONSE, hosted_rpc_decode(in, 4, &m));
}
