#include <stdint.h>
#include <string.h>

#include "radio_mux_spinel_uint.h"
#include "unity.h"

/* ---- radio_mux_spinel_uint_pack / radio_mux_spinel_uint_unpack ---- */

TEST_CASE("On values of one, two and three bytes: unpack returns what pack wrote, with its length", "[radio_mux]")
{
    const struct {
        uint32_t v;
        size_t len;
    } cases[] = {{0, 1}, {1, 1}, {127, 1}, {128, 2}, {0x1234, 2}, {16383, 2}, {16384, 3}, {RADIO_MUX_SPINEL_UINT_MAX, 3}};

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        uint8_t buf[RADIO_MUX_SPINEL_UINT_LEN_MAX];
        uint32_t back = 0xFFFFFFFF;

        TEST_ASSERT_EQUAL(cases[i].len, radio_mux_spinel_uint_pack(cases[i].v, buf));
        TEST_ASSERT_EQUAL(cases[i].len, radio_mux_spinel_uint_unpack(buf, sizeof(buf), &back));
        TEST_ASSERT_EQUAL_UINT32(cases[i].v, back);
    }
}

TEST_CASE("On a known encoding: pack writes the spinel bytes (low 7 bits first, high bit on all but the last)",
          "[radio_mux]")
{
    uint8_t buf[RADIO_MUX_SPINEL_UINT_LEN_MAX];

    TEST_ASSERT_EQUAL(2, radio_mux_spinel_uint_pack(0x3C00, buf)); // SPINEL_PROP_VENDOR__BEGIN
    TEST_ASSERT_EQUAL_HEX8(0x80, buf[0]);
    TEST_ASSERT_EQUAL_HEX8(0x78, buf[1]);
}

TEST_CASE("On the largest value: three bytes FF FF 7F", "[radio_mux]")
{
    const uint8_t max[] = {0xFF, 0xFF, 0x7F};
    uint8_t buf[RADIO_MUX_SPINEL_UINT_LEN_MAX];
    uint32_t v = 0;

    TEST_ASSERT_EQUAL(3, radio_mux_spinel_uint_pack(RADIO_MUX_SPINEL_UINT_MAX, buf));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(max, buf, sizeof(max));
    TEST_ASSERT_EQUAL(3, radio_mux_spinel_uint_unpack(max, sizeof(max), &v));
    TEST_ASSERT_EQUAL_UINT32(2097151, v);
}

TEST_CASE("On a value above the spinel limit: pack writes nothing and returns 0", "[radio_mux]")
{
    uint8_t buf[RADIO_MUX_SPINEL_UINT_LEN_MAX] = {0xAA, 0xAA, 0xAA};

    TEST_ASSERT_EQUAL(0, radio_mux_spinel_uint_pack(RADIO_MUX_SPINEL_UINT_MAX + 1, buf));
    TEST_ASSERT_EQUAL(0, radio_mux_spinel_uint_pack(UINT32_MAX, buf));
    TEST_ASSERT_EQUAL_HEX8(0xAA, buf[0]);
}

TEST_CASE("On an encoding longer than three bytes: unpack returns 0 and leaves the value", "[radio_mux]")
{
    const uint8_t four[] = {0x80, 0x80, 0x80, 0x01};
    const uint8_t five[] = {0xFF, 0xFF, 0xFF, 0xFF, 0x0F};
    uint32_t v = 7;

    TEST_ASSERT_EQUAL(0, radio_mux_spinel_uint_unpack(four, sizeof(four), &v));
    TEST_ASSERT_EQUAL(0, radio_mux_spinel_uint_unpack(five, sizeof(five), &v));
    TEST_ASSERT_EQUAL_UINT32(7, v);
}

TEST_CASE("On input that ends inside the number: unpack returns 0", "[radio_mux]")
{
    const uint8_t cut[] = {0x80, 0x81};
    uint32_t v = 7;

    TEST_ASSERT_EQUAL(0, radio_mux_spinel_uint_unpack(cut, sizeof(cut), &v));
    TEST_ASSERT_EQUAL(0, radio_mux_spinel_uint_unpack(cut, 0, &v));
    TEST_ASSERT_EQUAL_UINT32(7, v);
}

TEST_CASE("On bytes after the number: unpack consumes only the number", "[radio_mux]")
{
    const uint8_t in[] = {0x81, 0x01, 0x55};
    uint32_t v = 0;

    TEST_ASSERT_EQUAL(2, radio_mux_spinel_uint_unpack(in, sizeof(in), &v));
    TEST_ASSERT_EQUAL_UINT32(129, v);
}
