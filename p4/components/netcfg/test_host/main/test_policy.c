#include "unity.h"
#include "unity_test_runner.h"
#include "netcfg_policy.h"

// Policy table (spec 5.13): one test per mode x side row, the four services checked in each.
static void assert_row(netcfg_mode_t mode, netcfg_side_t side, bool gui, bool device_api, bool rest_read, bool rest_write)
{
    TEST_ASSERT_EQUAL_MESSAGE(gui, netcfg_policy_allows(mode, side, NETCFG_SVC_GUI), "GUI");
    TEST_ASSERT_EQUAL_MESSAGE(device_api, netcfg_policy_allows(mode, side, NETCFG_SVC_DEVICE_API), "DEVICE_API");
    TEST_ASSERT_EQUAL_MESSAGE(rest_read, netcfg_policy_allows(mode, side, NETCFG_SVC_REST_READ), "REST_READ");
    TEST_ASSERT_EQUAL_MESSAGE(rest_write, netcfg_policy_allows(mode, side, NETCFG_SVC_REST_WRITE), "REST_WRITE");
}

TEST_CASE("On a request arriving on mgmt in split mode: GUI, DEVICE_API and REST_READ are allowed, REST_WRITE is not", "[netcfg]")
{
    assert_row(NETCFG_MODE_SPLIT, NETCFG_SIDE_MGMT, true, true, true, false);
}

TEST_CASE("On a request arriving on thread_bb in split mode: REST_READ and REST_WRITE are allowed, GUI and DEVICE_API are not", "[netcfg]")
{
    assert_row(NETCFG_MODE_SPLIT, NETCFG_SIDE_THREAD, false, false, true, true);
}

TEST_CASE("On any request in single mode on the management side: every service is allowed", "[netcfg]")
{
    assert_row(NETCFG_MODE_SINGLE, NETCFG_SIDE_MGMT, true, true, true, true);
}

TEST_CASE("On any request in single mode on the Thread side: every service is allowed", "[netcfg]")
{
    assert_row(NETCFG_MODE_SINGLE, NETCFG_SIDE_THREAD, true, true, true, true);
}

TEST_CASE("On a confirm: only the management side may confirm", "[netcfg]")
{
    TEST_ASSERT_TRUE(netcfg_confirm_allowed(NETCFG_SIDE_MGMT));
    TEST_ASSERT_FALSE(netcfg_confirm_allowed(NETCFG_SIDE_THREAD));
}

TEST_CASE("On an IPv4-mapped IPv6 local address: the IPv4 address is extracted", "[netcfg]")
{
    const uint8_t mapped[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 192, 168, 7, 9};
    const uint8_t native[16] = {0xfd, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 192, 168, 7, 9};
    esp_ip4_addr_t out = {0};

    TEST_ASSERT_TRUE(netcfg_v4_from_mapped(mapped, &out));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(((const uint8_t[]) {
        192, 168, 7, 9
    }), (const uint8_t *)&out.addr, 4);
    TEST_ASSERT_FALSE(netcfg_v4_from_mapped(native, &out));
}

TEST_CASE("On an RCP proxy connection arriving on mgmt in split mode: it is allowed", "[netcfg]")
{
    TEST_ASSERT_TRUE(netcfg_policy_allows(NETCFG_MODE_SPLIT, NETCFG_SIDE_MGMT, NETCFG_SVC_RCP_PROXY));
}

TEST_CASE("On an RCP proxy connection arriving on thread_bb in split mode: it is allowed", "[netcfg]")
{
    TEST_ASSERT_TRUE(netcfg_policy_allows(NETCFG_MODE_SPLIT, NETCFG_SIDE_THREAD, NETCFG_SVC_RCP_PROXY));
}

TEST_CASE("On an RCP proxy connection in single mode: it is allowed on either side", "[netcfg]")
{
    TEST_ASSERT_TRUE(netcfg_policy_allows(NETCFG_MODE_SINGLE, NETCFG_SIDE_MGMT, NETCFG_SVC_RCP_PROXY));
    TEST_ASSERT_TRUE(netcfg_policy_allows(NETCFG_MODE_SINGLE, NETCFG_SIDE_THREAD, NETCFG_SVC_RCP_PROXY));
}

/* ---- netcfg_policy_conn_allowed: the decision of netcfg_peer_allowed (UDP sources) and netcfg_socket_allowed ---- */

TEST_CASE("On a source on the Thread mesh: returns false, in either mode and whether its side is known", "[netcfg]")
{
    TEST_ASSERT_FALSE(netcfg_policy_conn_allowed(NETCFG_MODE_SINGLE, true, NETCFG_SIDE_THREAD, true, NETCFG_SVC_RCP_PROXY));
    TEST_ASSERT_FALSE(netcfg_policy_conn_allowed(NETCFG_MODE_SINGLE, false, NETCFG_SIDE_MGMT, true, NETCFG_SVC_RCP_PROXY));
    TEST_ASSERT_FALSE(netcfg_policy_conn_allowed(NETCFG_MODE_SPLIT, true, NETCFG_SIDE_THREAD, true, NETCFG_SVC_RCP_PROXY));
    TEST_ASSERT_FALSE(netcfg_policy_conn_allowed(NETCFG_MODE_SPLIT, false, NETCFG_SIDE_MGMT, true, NETCFG_SVC_RCP_PROXY));
}

TEST_CASE("On split mode and a source on a side where the service is not allowed: returns false", "[netcfg]")
{
    // NETCFG_SVC_RCP_PROXY is allowed on both sides today; the rule is the policy table's, whatever the service.
    TEST_ASSERT_FALSE(netcfg_policy_conn_allowed(NETCFG_MODE_SPLIT, true, NETCFG_SIDE_MGMT, false, NETCFG_SVC_REST_WRITE));
    TEST_ASSERT_FALSE(netcfg_policy_conn_allowed(NETCFG_MODE_SPLIT, true, NETCFG_SIDE_THREAD, false, NETCFG_SVC_GUI));
}

TEST_CASE("On split mode and an RCP proxy source on either side: returns true", "[netcfg]")
{
    TEST_ASSERT_TRUE(netcfg_policy_conn_allowed(NETCFG_MODE_SPLIT, true, NETCFG_SIDE_MGMT, false, NETCFG_SVC_RCP_PROXY));
    TEST_ASSERT_TRUE(netcfg_policy_conn_allowed(NETCFG_MODE_SPLIT, true, NETCFG_SIDE_THREAD, false, NETCFG_SVC_RCP_PROXY));
}

TEST_CASE("On split mode and a source on no known interface: returns false", "[netcfg]")
{
    TEST_ASSERT_FALSE(netcfg_policy_conn_allowed(NETCFG_MODE_SPLIT, false, NETCFG_SIDE_MGMT, false, NETCFG_SVC_RCP_PROXY));
}

TEST_CASE("On single mode and a source off the mesh: returns true, also on no known interface", "[netcfg]")
{
    TEST_ASSERT_TRUE(netcfg_policy_conn_allowed(NETCFG_MODE_SINGLE, true, NETCFG_SIDE_MGMT, false, NETCFG_SVC_RCP_PROXY));
    TEST_ASSERT_TRUE(netcfg_policy_conn_allowed(NETCFG_MODE_SINGLE, false, NETCFG_SIDE_MGMT, false, NETCFG_SVC_RCP_PROXY));
}

TEST_CASE("On a local address on the OpenThread netif: the connection is refused", "[netcfg]")
{
    TEST_ASSERT_TRUE(netcfg_mesh_refused(true));
}

TEST_CASE("On a local address on an Ethernet interface: the connection is not refused", "[netcfg]")
{
    TEST_ASSERT_FALSE(netcfg_mesh_refused(false));
}
