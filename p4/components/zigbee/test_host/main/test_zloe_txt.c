#include <string.h>

#include "unity.h"
#include "zigbee_zloe_txt.h"

static const zigbee_zloe_record_t RECORD = {
    .eui64 = {0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28},
    .epan = {1, 2, 3, 4, 5, 6, 7, 8},
    .nwk = 0x0ba7,
};

static const char *value(const zigbee_zloe_txt_t *items, const char *key)
{
    for (int i = 0; i < ZIGBEE_ZLOE_TXT_ITEMS; i++) {
        if (strcmp(items[i].key, key) == 0) {
            return items[i].value;
        }
    }
    return NULL;
}

TEST_CASE("build, router with identity: writes the keys the host reads and no mc", "[zigbee]")
{
    zigbee_zloe_txt_t items[ZIGBEE_ZLOE_TXT_ITEMS];
    zigbee_zloe_txt_buf_t buf;

    zigbee_zloe_txt_build(&RECORD, items, &buf);
    TEST_ASSERT_EQUAL_STRING("2", value(items, "v"));
    TEST_ASSERT_EQUAL_STRING("2827262524232221", value(items, "xa"));
    TEST_ASSERT_EQUAL_STRING("0807060504030201", value(items, "xp"));
    TEST_ASSERT_EQUAL_STRING("0ba7", value(items, "nwk"));
    TEST_ASSERT_EQUAL_STRING("router", value(items, "role"));
    TEST_ASSERT_EQUAL_STRING("none", value(items, "sec"));
    TEST_ASSERT_NULL(value(items, "mc"));
}

TEST_CASE("parse, complete record: yields EUI64, extended PAN, NWK and role", "[zigbee]")
{
    const zigbee_zloe_txt_t items[] = {
        {"xa", "2827262524232221"}, {"xp", "0807060504030201"}, {"nwk", "0000"},
        {"role", "coordinator"},    {"sec", "none"},           {"v", "2"},
    };
    zigbee_zloe_record_t out;

    TEST_ASSERT_TRUE(zigbee_zloe_txt_parse(items, 6, &out));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(RECORD.eui64, out.eui64, 8);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(RECORD.epan, out.epan, 8);
    TEST_ASSERT_EQUAL_HEX16(0, out.nwk);
    TEST_ASSERT_TRUE(out.coordinator);
}

TEST_CASE("parse, missing or malformed xa: rejects the record", "[zigbee]")
{
    zigbee_zloe_txt_t items[] = {
        {"xp", "0807060504030201"}, {"nwk", "0000"}, {"sec", "none"}, {"xa", "zz27262524232221"},
    };
    zigbee_zloe_record_t out;

    TEST_ASSERT_FALSE(zigbee_zloe_txt_parse(items, 3, &out));
    TEST_ASSERT_FALSE(zigbee_zloe_txt_parse(items, 4, &out));
}

TEST_CASE("parse, sec other than none: rejects the record", "[zigbee]")
{
    const zigbee_zloe_txt_t items[] = {
        {"xa", "2827262524232221"}, {"xp", "0807060504030201"}, {"nwk", "0000"}, {"sec", "quic"},
    };
    zigbee_zloe_record_t out;

    TEST_ASSERT_FALSE(zigbee_zloe_txt_parse(items, 4, &out));
}

TEST_CASE("keep_link_local, mixed addresses: keeps only the link-local ones in order", "[zigbee]")
{
    uint8_t addrs[3][16] = {{0x2a, 0x06}, {0xfe, 0x80, 1}, {0xfe, 0xbf, 2}};

    TEST_ASSERT_EQUAL(2, zigbee_zloe_keep_link_local(addrs, 3));
    TEST_ASSERT_EQUAL(1, addrs[0][2]);
    TEST_ASSERT_EQUAL(2, addrs[1][2]);
}

TEST_CASE("keep_link_local, only routable: keeps none", "[zigbee]")
{
    uint8_t addrs[1][16] = {{0x2a, 0x06}};

    TEST_ASSERT_EQUAL(0, zigbee_zloe_keep_link_local(addrs, 1));
}
