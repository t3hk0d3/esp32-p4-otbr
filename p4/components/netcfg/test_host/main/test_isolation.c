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
