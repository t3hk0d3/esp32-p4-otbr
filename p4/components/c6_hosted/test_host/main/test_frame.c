#include <string.h>
#include "unity.h"
#include "hosted_frame.h"

static uint16_t u16_at(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint16_t sum_without_checksum(const uint8_t *p, size_t len)
{
    uint16_t sum = 0;
    for (size_t i = 0; i < len; i++) {
        sum += (i == 6 || i == 7) ? 0 : p[i];
    }
    return sum;
}

/* ---- hosted_frame_encode ---- */

TEST_CASE("On a serial RPC payload: if_type 3, offset 12, len and the checksum as the u16 byte sum", "[frame]")
{
    const uint8_t payload[] = {1, 2, 3, 0xFE, 0xFF};
    uint8_t out[64];

    const size_t n = hosted_frame_encode(HOSTED_IF_SERIAL, 0x1234, payload, sizeof(payload), out, sizeof(out));
    TEST_ASSERT_EQUAL(HOSTED_HEADER_LEN + sizeof(payload), n);
    TEST_ASSERT_EQUAL_HEX8(HOSTED_IF_SERIAL, out[0]);
    TEST_ASSERT_EQUAL_UINT16(sizeof(payload), u16_at(out + 2));
    TEST_ASSERT_EQUAL_UINT16(HOSTED_HEADER_LEN, u16_at(out + 4));
    TEST_ASSERT_EQUAL_UINT16(0x1234, u16_at(out + 8));
    TEST_ASSERT_EQUAL_UINT16(sum_without_checksum(out, n), u16_at(out + 6));
    TEST_ASSERT_EQUAL_MEMORY(payload, out + HOSTED_HEADER_LEN, sizeof(payload));
}

TEST_CASE("On a packet longer than the buffer or HOSTED_MAX_PACKET: returns 0", "[frame]")
{
    static uint8_t payload[HOSTED_MAX_PACKET];
    static uint8_t out[2 * HOSTED_MAX_PACKET];

    TEST_ASSERT_EQUAL(0, hosted_frame_encode(HOSTED_IF_SERIAL, 0, payload, 10, out, 15));
    TEST_ASSERT_EQUAL(0, hosted_frame_encode(HOSTED_IF_SERIAL, 0, payload, HOSTED_MAX_PACKET - HOSTED_HEADER_LEN + 1, out,
                                             sizeof(out)));
}

/* ---- hosted_frame_next ---- */

TEST_CASE("On a stream holding three packets: yields them in order with their lengths", "[frame]")
{
    uint8_t stream[256];
    size_t len = 0;
    const uint8_t a[] = {0xAA}, b[] = {0xBB, 0xBB}, c[] = {0xCC, 0xCC, 0xCC};
    len += hosted_frame_encode(HOSTED_IF_PRIV, 1, a, sizeof(a), stream + len, sizeof(stream) - len);
    len += hosted_frame_encode(HOSTED_IF_SERIAL, 2, b, sizeof(b), stream + len, sizeof(stream) - len);
    len += hosted_frame_encode(HOSTED_IF_SERIAL, 3, c, sizeof(c), stream + len, sizeof(stream) - len);

    hosted_packet_t p;
    size_t pos = 0;
    const uint8_t types[] = {HOSTED_IF_PRIV, HOSTED_IF_SERIAL, HOSTED_IF_SERIAL};
    for (int i = 0; i < 3; i++) {
        const int n = hosted_frame_next(stream + pos, len - pos, &p);
        TEST_ASSERT_EQUAL(HOSTED_HEADER_LEN + i + 1, n);
        TEST_ASSERT_EQUAL(types[i], p.if_type);
        TEST_ASSERT_EQUAL(i + 1, p.seq);
        TEST_ASSERT_EQUAL(i + 1, p.len);
        pos += (size_t)n;
    }
    TEST_ASSERT_EQUAL(len, pos);
}

TEST_CASE("On a truncated packet: returns 0 (wait for more)", "[frame]")
{
    uint8_t buf[64];
    const uint8_t payload[10] = {0};
    const size_t n = hosted_frame_encode(HOSTED_IF_SERIAL, 0, payload, sizeof(payload), buf, sizeof(buf));
    hosted_packet_t p;

    TEST_ASSERT_EQUAL(0, hosted_frame_next(buf, n - 1, &p));
    TEST_ASSERT_EQUAL(0, hosted_frame_next(buf, HOSTED_HEADER_LEN - 1, &p));
}

TEST_CASE("On a bad offset or all-zero block padding: returns -1", "[frame]")
{
    uint8_t buf[32] = {0};
    hosted_packet_t p;

    TEST_ASSERT_EQUAL(-1, hosted_frame_next(buf, sizeof(buf), &p));
    buf[2] = 4;
    buf[4] = 8; /* offset below the header */
    TEST_ASSERT_EQUAL(-1, hosted_frame_next(buf, sizeof(buf), &p));
    buf[4] = HOSTED_HEADER_LEN;
    buf[2] = 0xFF;
    buf[3] = 0x0F; /* longer than a packet */
    TEST_ASSERT_EQUAL(-1, hosted_frame_next(buf, sizeof(buf), &p));
}

/* ---- serial TLV ---- */

TEST_CASE("On wrapping RPC bytes: the TLV carries endpoint RPCRsp and unwraps to the same bytes", "[frame]")
{
    const uint8_t proto[] = {0x08, 0x01, 0x10, 0x90, 0x02};
    uint8_t buf[64];
    const size_t n = hosted_serial_wrap(proto, sizeof(proto), buf, sizeof(buf));

    TEST_ASSERT_EQUAL(HOSTED_SERIAL_OVERHEAD + sizeof(proto), n);
    TEST_ASSERT_EQUAL_MEMORY("RPCRsp", buf + 3, 6);
    bool is_event = true;
    const uint8_t *out = NULL;
    size_t out_len = 0;
    TEST_ASSERT_EQUAL(ESP_OK, hosted_serial_unwrap(buf, n, &is_event, &out, &out_len));
    TEST_ASSERT_FALSE(is_event);
    TEST_ASSERT_EQUAL(sizeof(proto), out_len);
    TEST_ASSERT_EQUAL_MEMORY(proto, out, sizeof(proto));
}

TEST_CASE("On an RPCEvt TLV: unwraps as an event; another endpoint or a short buffer is rejected", "[frame]")
{
    const uint8_t proto[] = {0x08, 0x03};
    uint8_t buf[64];
    const size_t n = hosted_serial_wrap(proto, sizeof(proto), buf, sizeof(buf));
    bool is_event = false;
    const uint8_t *out = NULL;
    size_t out_len = 0;

    memcpy(buf + 3, "RPCEvt", 6);
    TEST_ASSERT_EQUAL(ESP_OK, hosted_serial_unwrap(buf, n, &is_event, &out, &out_len));
    TEST_ASSERT_TRUE(is_event);
    memcpy(buf + 3, "RPCxxx", 6);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_RESPONSE, hosted_serial_unwrap(buf, n, &is_event, &out, &out_len));
    memcpy(buf + 3, "RPCRsp", 6);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_RESPONSE, hosted_serial_unwrap(buf, n - 1, &is_event, &out, &out_len));
}

/* ---- PRIV startup event ---- */

TEST_CASE("On the PRIV init event: returns the chip ID TLV, skipping other TLVs", "[frame]")
{
    const uint8_t ev[] = {HOSTED_PRIV_EVENT_INIT, 7, 0x11, 1, 0x3F, HOSTED_PRIV_TAG_CHIP_ID, 1, HOSTED_CHIP_ID_ESP32C6, 0};
    const hosted_packet_t pkt = {
        .if_type = HOSTED_IF_PRIV, .pkt_type = HOSTED_PRIV_PKT_EVENT, .payload = ev, .len = sizeof(ev),
    };
    uint8_t chip = 0;

    TEST_ASSERT_EQUAL(ESP_OK, hosted_priv_init_chip_id(&pkt, &chip));
    TEST_ASSERT_EQUAL_HEX8(HOSTED_CHIP_ID_ESP32C6, chip);
}

TEST_CASE("On another PRIV packet or an init event without the chip TLV: ESP_ERR_NOT_FOUND", "[frame]")
{
    const uint8_t ev[] = {HOSTED_PRIV_EVENT_INIT, 3, 0x11, 1, 0x3F};
    hosted_packet_t pkt = {.if_type = HOSTED_IF_PRIV, .pkt_type = HOSTED_PRIV_PKT_EVENT, .payload = ev, .len = sizeof(ev)};
    uint8_t chip = 0;

    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, hosted_priv_init_chip_id(&pkt, &chip));
    pkt.if_type = HOSTED_IF_SERIAL;
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, hosted_priv_init_chip_id(&pkt, &chip));
}
