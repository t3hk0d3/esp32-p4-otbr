#include <stdlib.h>
#include <string.h>
#include "unity.h"
#include "unity_test_runner.h"
#include "netcfg_vlan.h"

#define MAC_PAIR_LEN 12

static const uint8_t k_macs[MAC_PAIR_LEN] = {1, 2, 3, 4, 5, 6, 0xa, 0xb, 0xc, 0xd, 0xe, 0xf};

// Frame with `payload_len` bytes after the ethertype 0x86dd; tagged when vid >= 0.
static size_t build(uint8_t *buf, int vid, size_t payload_len)
{
    size_t n = 0;

    memcpy(buf, k_macs, MAC_PAIR_LEN);
    n = MAC_PAIR_LEN;
    if (vid >= 0) {
        netcfg_vlan_tag((uint16_t)vid, buf + n);
        n += NETCFG_VLAN_TAG_LEN;
    }
    buf[n++] = 0x86;
    buf[n++] = 0xdd;
    for (size_t i = 0; i < payload_len; i++) {
        buf[n++] = (uint8_t)(0x40 + i);
    }
    return n;
}

static void assert_untagged_frame(const netcfg_rx_result_t *r, const uint8_t *buf, size_t payload_len)
{
    TEST_ASSERT_EQUAL_HEX8_ARRAY(k_macs, r->frame, MAC_PAIR_LEN);
    TEST_ASSERT_EQUAL_HEX8(0x86, r->frame[MAC_PAIR_LEN]);
    TEST_ASSERT_EQUAL_HEX8(0xdd, r->frame[MAC_PAIR_LEN + 1]);
    TEST_ASSERT_EQUAL(MAC_PAIR_LEN + 2 + payload_len, r->len);
    for (size_t i = 0; i < payload_len; i++) {
        TEST_ASSERT_EQUAL_HEX8(0x40 + i, r->frame[MAC_PAIR_LEN + 2 + i]);
    }
    (void)buf;
}

TEST_CASE("On the tag of VLAN ID 20: TPID 0x8100, priority 0, ID in the low 12 bits", "[netcfg]")
{
    uint8_t tag[NETCFG_VLAN_TAG_LEN];

    netcfg_vlan_tag(20, tag);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(((const uint8_t[]) {
        0x81, 0x00, 0x00, 0x14
    }), tag, 4);
    netcfg_vlan_tag(4094, tag);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(((const uint8_t[]) {
        0x81, 0x00, 0x0f, 0xfe
    }), tag, 4);
}

TEST_CASE("On single mode untagged: an untagged frame is delivered unchanged to the only interface", "[netcfg]")
{
    const netcfg_demux_t table = {.count = 1, .vlan = {0}};
    uint8_t buf[64];
    const size_t len = build(buf, -1, 20);
    const netcfg_rx_result_t r = netcfg_demux(&table, buf, len);

    TEST_ASSERT_EQUAL(NETCFG_RX_DELIVER, r.verdict);
    TEST_ASSERT_EQUAL(0, r.iface);
    TEST_ASSERT_TRUE(r.frame == buf);
    assert_untagged_frame(&r, buf, 20);
}

TEST_CASE("On a frame with a matching VLAN ID: the tag is removed and the interface is chosen", "[netcfg]")
{
    const netcfg_demux_t table = {.count = 2, .vlan = {10, 20}};
    uint8_t buf[64];
    const size_t len = build(buf, 20, 20);
    const netcfg_rx_result_t r = netcfg_demux(&table, buf, len);

    TEST_ASSERT_EQUAL(NETCFG_RX_DELIVER, r.verdict);
    TEST_ASSERT_EQUAL(1, r.iface);
    TEST_ASSERT_TRUE(r.frame == buf + NETCFG_VLAN_TAG_LEN); // the interface frees frame - 4
    assert_untagged_frame(&r, buf, 20);
}

TEST_CASE("On a frame whose priority bits are set: the VLAN ID alone decides", "[netcfg]")
{
    const netcfg_demux_t table = {.count = 1, .vlan = {20}};
    uint8_t buf[64];
    const size_t len = build(buf, 20, 8);

    buf[MAC_PAIR_LEN + 2] |= 0xe0; // PCP 7
    TEST_ASSERT_EQUAL(NETCFG_RX_DELIVER, netcfg_demux(&table, buf, len).verdict);
}

TEST_CASE("On split mode with one untagged interface: untagged frames go to it and tagged frames to the other", "[netcfg]")
{
    const netcfg_demux_t table = {.count = 2, .vlan = {0, 20}};
    uint8_t buf[64];
    size_t len = build(buf, -1, 8);
    netcfg_rx_result_t r = netcfg_demux(&table, buf, len);

    TEST_ASSERT_EQUAL(NETCFG_RX_DELIVER, r.verdict);
    TEST_ASSERT_EQUAL(0, r.iface);
    len = build(buf, 20, 8);
    r = netcfg_demux(&table, buf, len);
    TEST_ASSERT_EQUAL(NETCFG_RX_DELIVER, r.verdict);
    TEST_ASSERT_EQUAL(1, r.iface);
}

TEST_CASE("On a frame with an unknown VLAN ID: it is dropped", "[netcfg]")
{
    const netcfg_demux_t table = {.count = 2, .vlan = {0, 20}};
    uint8_t buf[64];
    const size_t len = build(buf, 30, 8);

    TEST_ASSERT_EQUAL(NETCFG_RX_DROP_UNKNOWN_VLAN, netcfg_demux(&table, buf, len).verdict);
}

TEST_CASE("On a priority-tagged frame (VLAN ID 0): it is dropped as an unknown VLAN, also with an untagged interface", "[netcfg]")
{
    const netcfg_demux_t table = {.count = 1, .vlan = {0}};
    uint8_t buf[64];
    const size_t len = build(buf, 0, 8);

    TEST_ASSERT_EQUAL(NETCFG_RX_DROP_UNKNOWN_VLAN, netcfg_demux(&table, buf, len).verdict);
}

TEST_CASE("On an untagged frame when no interface is untagged: it is dropped", "[netcfg]")
{
    const netcfg_demux_t table = {.count = 2, .vlan = {10, 20}};
    uint8_t buf[64];
    const size_t len = build(buf, -1, 8);

    TEST_ASSERT_EQUAL(NETCFG_RX_DROP_NO_UNTAGGED, netcfg_demux(&table, buf, len).verdict);
}

TEST_CASE("On a tag header cut short: it is dropped as malformed", "[netcfg]")
{
    const netcfg_demux_t table = {.count = 1, .vlan = {20}};
    uint8_t buf[64];

    build(buf, 20, 8);
    TEST_ASSERT_EQUAL(NETCFG_RX_DROP_MALFORMED, netcfg_demux(&table, buf, MAC_PAIR_LEN + 2).verdict);
    TEST_ASSERT_EQUAL(NETCFG_RX_DROP_MALFORMED, netcfg_demux(&table, buf, MAC_PAIR_LEN + NETCFG_VLAN_TAG_LEN).verdict);
}

void app_main(void)
{
    UNITY_BEGIN();
    unity_run_all_tests();
    int failures = (int)UNITY_END();
    exit(failures == 0 ? 0 : 1);
}
