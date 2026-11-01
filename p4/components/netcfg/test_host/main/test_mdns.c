#include <string.h>
#include "unity.h"
#include "unity_test_runner.h"
#include "netcfg_mdns.h"

static netcfg_addr_t v4(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
    netcfg_addr_t addr = {.is_v6 = false, .bytes = {a, b, c, d}};

    return addr;
}

static netcfg_addr_t v6(const uint8_t bytes[16])
{
    netcfg_addr_t addr = {.is_v6 = true};

    memcpy(addr.bytes, bytes, sizeof(addr.bytes));
    return addr;
}

static const uint8_t MGMT6[16] = {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 10, 0, 0, 0, 0, 0, 0, 0, 1};
static const uint8_t BB6[16] = {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 20, 0, 0, 0, 0, 0, 0, 0, 1};
static const uint8_t LL6[16] = {0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0x02, 0x11, 0x22, 0xff, 0xfe, 0x33, 0x44, 0x55};
static const uint8_t PEER6[16] = {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 20, 0, 0, 0, 0, 0, 0, 0, 0x99};
static const uint8_t MAPPED_MGMT[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 10, 0, 10, 5};

TEST_CASE("Single mode, the only interface: mDNS is allowed", "[netcfg]")
{
    TEST_ASSERT_TRUE(netcfg_mdns_allowed(NETCFG_MODE_SINGLE, NETCFG_SIDE_MGMT));
}

TEST_CASE("Split mode, thread_bb: mDNS is allowed", "[netcfg]")
{
    TEST_ASSERT_TRUE(netcfg_mdns_allowed(NETCFG_MODE_SPLIT, NETCFG_SIDE_THREAD));
}

TEST_CASE("Split mode, mgmt: mDNS is not allowed", "[netcfg]")
{
    TEST_ASSERT_FALSE(netcfg_mdns_allowed(NETCFG_MODE_SPLIT, NETCFG_SIDE_MGMT));
}

TEST_CASE("IPv4 source that is an address of the other interface: is our own announcement", "[netcfg]")
{
    const netcfg_addr_t src = v4(10, 0, 10, 5);
    const netcfg_addr_t input[] = {v4(10, 0, 20, 5)};
    const netcfg_addr_t other[] = {v4(10, 0, 10, 5)};

    TEST_ASSERT_TRUE(netcfg_mdns_source_is_foreign_own(&src, input, 1, other, 1));
}

TEST_CASE("IPv6 global source that is an address of the other interface: is our own announcement", "[netcfg]")
{
    const netcfg_addr_t src = v6(MGMT6);
    const netcfg_addr_t input[] = {v6(BB6)};
    const netcfg_addr_t other[] = {v6(MGMT6)};

    TEST_ASSERT_TRUE(netcfg_mdns_source_is_foreign_own(&src, input, 1, other, 1));
}

TEST_CASE("IPv6 link-local source that is only an address of the other interface: is our own announcement", "[netcfg]")
{
    const netcfg_addr_t src = v6(LL6);
    const netcfg_addr_t input[] = {v6(BB6)};
    const netcfg_addr_t other[] = {v6(MGMT6), v6(LL6)};

    TEST_ASSERT_TRUE(netcfg_mdns_source_is_foreign_own(&src, input, 1, other, 2));
}

TEST_CASE("IPv4-mapped IPv6 source that is an IPv4 address of the other interface: is our own announcement", "[netcfg]")
{
    const netcfg_addr_t src = v6(MAPPED_MGMT);
    const netcfg_addr_t other[] = {v4(10, 0, 10, 5)};

    TEST_ASSERT_TRUE(netcfg_mdns_source_is_foreign_own(&src, NULL, 0, other, 1));
}

TEST_CASE("Source that is an address of the interface it arrived on: is not ignored (normal loopback)", "[netcfg]")
{
    const netcfg_addr_t src = v4(10, 0, 20, 5);
    const netcfg_addr_t input[] = {v4(10, 0, 20, 5)};
    const netcfg_addr_t other[] = {v4(10, 0, 10, 5)};

    TEST_ASSERT_FALSE(netcfg_mdns_source_is_foreign_own(&src, input, 1, other, 1));
}

TEST_CASE("Source that is an address of both interfaces (shared link-local): is not ignored", "[netcfg]")
{
    const netcfg_addr_t src = v6(LL6);
    const netcfg_addr_t input[] = {v6(LL6)};
    const netcfg_addr_t other[] = {v6(LL6)};

    TEST_ASSERT_FALSE(netcfg_mdns_source_is_foreign_own(&src, input, 1, other, 1));
}

TEST_CASE("Source of another host: is not ignored", "[netcfg]")
{
    const netcfg_addr_t src4 = v4(10, 0, 20, 77);
    const netcfg_addr_t src6 = v6(PEER6);
    const netcfg_addr_t other[] = {v4(10, 0, 10, 5), v6(MGMT6)};

    TEST_ASSERT_FALSE(netcfg_mdns_source_is_foreign_own(&src4, NULL, 0, other, 2));
    TEST_ASSERT_FALSE(netcfg_mdns_source_is_foreign_own(&src6, NULL, 0, other, 2));
}

TEST_CASE("Unspecified source while another interface has no address yet: is not ignored", "[netcfg]")
{
    const netcfg_addr_t src4 = v4(0, 0, 0, 0);
    const uint8_t zero[16] = {0};
    const netcfg_addr_t src6 = v6(zero);
    const netcfg_addr_t other[] = {v4(0, 0, 0, 0), v6(zero)};

    TEST_ASSERT_FALSE(netcfg_mdns_source_is_foreign_own(&src4, NULL, 0, other, 2));
    TEST_ASSERT_FALSE(netcfg_mdns_source_is_foreign_own(&src6, NULL, 0, other, 2));
}

TEST_CASE("Same bytes in the other address family: is not ignored", "[netcfg]")
{
    const netcfg_addr_t src = v4(0x20, 0x01, 0x0d, 0xb8);
    const netcfg_addr_t other[] = {v6(MGMT6)};

    TEST_ASSERT_FALSE(netcfg_mdns_source_is_foreign_own(&src, NULL, 0, other, 1));
}

TEST_CASE("No addresses on the other interfaces: nothing is ignored", "[netcfg]")
{
    const netcfg_addr_t src = v4(10, 0, 10, 5);

    TEST_ASSERT_FALSE(netcfg_mdns_source_is_foreign_own(&src, NULL, 0, NULL, 0));
}
