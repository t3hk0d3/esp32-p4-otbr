#include <string.h>
#include "unity.h"
#include "unity_test_runner.h"
#include "netcfg_isolation.h"

TEST_CASE("On a packet arriving on mgmt addressed to the device (own address or link-scope multicast): is kept", "[netcfg]")
{
    TEST_ASSERT_FALSE(netcfg_ip6_input_drop(true, NETCFG_DEST_OF_INPUT_IF, false));
    TEST_ASSERT_FALSE(netcfg_ip6_input_drop(true, NETCFG_DEST_MULTICAST_LINK, false));
}

TEST_CASE("On a packet arriving on mgmt not addressed to the device: is dropped, not forwarded", "[netcfg]")
{
    TEST_ASSERT_TRUE(netcfg_ip6_input_drop(true, NETCFG_DEST_REMOTE, false));
    TEST_ASSERT_TRUE(netcfg_ip6_input_drop(true, NETCFG_DEST_REMOTE, true));
    TEST_ASSERT_TRUE(netcfg_ip6_input_drop(true, NETCFG_DEST_MULTICAST_WIDE, false));
}

TEST_CASE("On a packet arriving on mgmt addressed to another interface of the device: is dropped", "[netcfg]")
{
    TEST_ASSERT_TRUE(netcfg_ip6_input_drop(true, NETCFG_DEST_OF_OTHER_IF, false));
}

TEST_CASE("On a packet from thread_bb or Thread whose route resolves to mgmt: is dropped, not forwarded", "[netcfg]")
{
    TEST_ASSERT_TRUE(netcfg_ip6_input_drop(false, NETCFG_DEST_REMOTE, true));
    TEST_ASSERT_TRUE(netcfg_ip6_input_drop(false, NETCFG_DEST_MULTICAST_WIDE, true));
}

TEST_CASE("On a packet from thread_bb or Thread addressed to a mgmt address: is dropped", "[netcfg]")
{
    TEST_ASSERT_TRUE(netcfg_ip6_input_drop(false, NETCFG_DEST_OF_MGMT_ONLY, false));
}

TEST_CASE("On a packet from thread_bb or Thread that does not touch mgmt: is left to the original hook", "[netcfg]")
{
    TEST_ASSERT_FALSE(netcfg_ip6_input_drop(false, NETCFG_DEST_REMOTE, false));
    TEST_ASSERT_FALSE(netcfg_ip6_input_drop(false, NETCFG_DEST_OF_INPUT_IF, false));
    TEST_ASSERT_FALSE(netcfg_ip6_input_drop(false, NETCFG_DEST_OF_OTHER_IF, false));
    TEST_ASSERT_FALSE(netcfg_ip6_input_drop(false, NETCFG_DEST_MULTICAST_LINK, false));
    TEST_ASSERT_FALSE(netcfg_ip6_input_drop(false, NETCFG_DEST_MULTICAST_WIDE, false));
}

TEST_CASE("On a reply to an off-subnet management client: leaves via mgmt", "[netcfg]")
{
    TEST_ASSERT_EQUAL(NETCFG_ROUTE_MGMT, netcfg_ip6_route_choice(true, true, false, true, false));
    TEST_ASSERT_EQUAL(NETCFG_ROUTE_MGMT, netcfg_ip6_route_choice(true, true, true, false, false));
}

TEST_CASE("On a source that is not mgmt whose route would be mgmt: it is not mgmt", "[netcfg]")
{
    TEST_ASSERT_EQUAL(NETCFG_ROUTE_BACKBONE, netcfg_ip6_route_choice(true, false, true, false, false));
}

TEST_CASE("On a source that is not mgmt with no route from the original hook: the backbone, unless another interface has it on-link", "[netcfg]")
{
    TEST_ASSERT_EQUAL(NETCFG_ROUTE_BACKBONE, netcfg_ip6_route_choice(true, false, false, true, false));
    TEST_ASSERT_EQUAL(NETCFG_ROUTE_ORIGINAL, netcfg_ip6_route_choice(true, false, false, true, true));
}

TEST_CASE("On a source that is not mgmt with a non-mgmt route from the original hook: the original answer stands", "[netcfg]")
{
    TEST_ASSERT_EQUAL(NETCFG_ROUTE_ORIGINAL, netcfg_ip6_route_choice(true, false, false, false, false));
}

TEST_CASE("On single mode: the original hook decides whatever the source", "[netcfg]")
{
    TEST_ASSERT_EQUAL(NETCFG_ROUTE_ORIGINAL, netcfg_ip6_route_choice(false, true, true, false, false));
    TEST_ASSERT_EQUAL(NETCFG_ROUTE_ORIGINAL, netcfg_ip6_route_choice(false, false, false, true, false));
}

TEST_CASE("On UDP to the TREL port arriving on mgmt: is dropped", "[netcfg]")
{
    TEST_ASSERT_TRUE(netcfg_ip6_udp_drop(true, 12390, 12390));
}

TEST_CASE("On UDP to another port arriving on mgmt: is kept", "[netcfg]")
{
    TEST_ASSERT_FALSE(netcfg_ip6_udp_drop(true, 80, 12390));
    TEST_ASSERT_FALSE(netcfg_ip6_udp_drop(true, 5353, 12390));
}

TEST_CASE("On UDP to the TREL port arriving on thread_bb or the single port: is kept", "[netcfg]")
{
    TEST_ASSERT_FALSE(netcfg_ip6_udp_drop(false, 12390, 12390));
}

TEST_CASE("On a build without TREL (port 0): nothing is dropped", "[netcfg]")
{
    TEST_ASSERT_FALSE(netcfg_ip6_udp_drop(true, 0, 0));
    TEST_ASSERT_FALSE(netcfg_ip6_udp_drop(true, 12390, 0));
}

// IPv6 packet: fixed header with `next`, then `ext` (already laid out), then an 8-byte UDP header to `port`.
static size_t build_packet(uint8_t *buf, uint8_t next, const uint8_t *ext, size_t ext_len, uint16_t port)
{
    memset(buf, 0, 40);
    buf[0] = 0x60;
    buf[6] = next;
    memcpy(buf + 40, ext, ext_len);
    uint8_t *udp = buf + 40 + ext_len;

    memset(udp, 0, 8);
    udp[2] = port >> 8;
    udp[3] = port & 0xff;
    return 40 + ext_len + 8;
}

TEST_CASE("On a UDP header right after the fixed header: its port is found", "[netcfg]")
{
    uint8_t buf[128];
    uint16_t port = 0;
    const size_t len = build_packet(buf, 17, NULL, 0, 12390);

    TEST_ASSERT_TRUE(netcfg_ip6_udp_dest_port(buf, len, &port));
    TEST_ASSERT_EQUAL_UINT16(12390, port);
}

TEST_CASE("On UDP behind Destination Options and Hop-by-Hop headers: its port is found", "[netcfg]")
{
    uint8_t buf[128];
    uint16_t port = 0;
    // Hop-by-Hop (8 bytes) -> Destination Options (16 bytes) -> UDP
    const uint8_t ext[24] = {60, 0, [8] = 17, 1};
    const size_t len = build_packet(buf, 0, ext, sizeof(ext), 12390);

    TEST_ASSERT_TRUE(netcfg_ip6_udp_dest_port(buf, len, &port));
    TEST_ASSERT_EQUAL_UINT16(12390, port);
}

TEST_CASE("On the first fragment of a UDP datagram: its port is found", "[netcfg]")
{
    uint8_t buf[128];
    uint16_t port = 0;
    const uint8_t ext[8] = {17, 0, 0x00, 0x01}; // offset 0, more fragments
    const size_t len = build_packet(buf, 44, ext, sizeof(ext), 12390);

    TEST_ASSERT_TRUE(netcfg_ip6_udp_dest_port(buf, len, &port));
    TEST_ASSERT_EQUAL_UINT16(12390, port);
}

TEST_CASE("On a later fragment, a header past the end, or no UDP: no port", "[netcfg]")
{
    uint8_t buf[128];
    uint16_t port = 0;
    const uint8_t later[8] = {17, 0, 0x00, 0x08}; // offset 8
    size_t len = build_packet(buf, 44, later, sizeof(later), 12390);

    TEST_ASSERT_FALSE(netcfg_ip6_udp_dest_port(buf, len, &port));
    len = build_packet(buf, 17, NULL, 0, 12390);
    TEST_ASSERT_FALSE(netcfg_ip6_udp_dest_port(buf, len - 1, &port));
    const uint8_t long_opts[8] = {17, 4}; // claims 40 bytes, only 8 + UDP present
    len = build_packet(buf, 60, long_opts, sizeof(long_opts), 12390);
    TEST_ASSERT_FALSE(netcfg_ip6_udp_dest_port(buf, len, &port));
    len = build_packet(buf, 6, NULL, 0, 12390); // TCP
    TEST_ASSERT_FALSE(netcfg_ip6_udp_dest_port(buf, len, &port));
    TEST_ASSERT_FALSE(netcfg_ip6_udp_dest_port(buf, 20, &port));
}
