#include <string.h>
#include "unity.h"
#include "time_json.h"

static timesync_status_t synced_from_dhcp(void)
{
    timesync_status_t st = {
        .config = {.enabled = true, .mode = TIMESYNC_MODE_AUTO},
        .state = TIMESYNC_STATE_SYNCED,
        .now = 1800000000,
        .synced_age_s = 240,
        .servers = {.count = 2, .addr = {"10.0.10.1", "10.0.10.2"}, .source = TIMESYNC_SOURCE_DHCP},
        .answered = 0,
        .dhcp = {.count = 2, .addr = {"10.0.10.1", "10.0.10.2"}, .iface = TIMESYNC_IFACE_MGMT},
    };

    return st;
}

static void assert_parse_rejected(const char *body, const char *want_field)
{
    timesync_config_t out;
    const char *field = NULL;

    TEST_ASSERT_EQUAL_MESSAGE(ESP_ERR_INVALID_ARG, device_api_time_parse(body, &out, &field), body);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(want_field, field, body);
}

/* ---- device_api_time_json ---- */

TEST_CASE("Time GET, synced from DHCP on mgmt: writes every field of spec 5.2", "[device_api]")
{
    const timesync_status_t st = synced_from_dhcp();
    char buf[512];

    TEST_ASSERT_GREATER_THAN(0, device_api_time_json(buf, sizeof(buf), &st));
    TEST_ASSERT_EQUAL_STRING("{\"enabled\":true,\"mode\":\"auto\",\"server\":\"\",\"state\":\"synced\","
                             "\"now\":1800000000,\"synced_age_s\":240,\"source\":\"dhcp\","
                             "\"servers\":[{\"address\":\"10.0.10.1\",\"answered\":true},"
                             "{\"address\":\"10.0.10.2\",\"answered\":false}],"
                             "\"dhcp_servers\":[\"10.0.10.1\",\"10.0.10.2\"],\"dhcp_iface\":\"mgmt\"}",
                             buf);
}

TEST_CASE("Time GET, clock not set: now and synced_age_s are null", "[device_api]")
{
    timesync_status_t st = synced_from_dhcp();
    char buf[512];

    st.state = TIMESYNC_STATE_WAITING;
    st.now = 0;
    st.synced_age_s = -1;
    st.answered = -1;
    TEST_ASSERT_GREATER_THAN(0, device_api_time_json(buf, sizeof(buf), &st));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"now\":null,\"synced_age_s\":null"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"answered\":false},{\"address\":\"10.0.10.2\",\"answered\":false}"));
}

TEST_CASE("Time GET, disabled: state off, source null, no servers", "[device_api]")
{
    timesync_status_t st = synced_from_dhcp();
    char buf[512];

    st.config.enabled = false;
    st.state = TIMESYNC_STATE_OFF;
    st.servers = (timesync_servers_t) {
        .source = TIMESYNC_SOURCE_NONE
    };
    st.answered = -1;
    TEST_ASSERT_GREATER_THAN(0, device_api_time_json(buf, sizeof(buf), &st));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"state\":\"off\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"source\":null,\"servers\":[]"));
}

TEST_CASE("Time GET, lease interface not known: dhcp_iface is null", "[device_api]")
{
    timesync_status_t st = synced_from_dhcp();
    char buf[512];

    st.dhcp.iface = TIMESYNC_IFACE_NONE;
    TEST_ASSERT_GREATER_THAN(0, device_api_time_json(buf, sizeof(buf), &st));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"dhcp_iface\":null}"));
}

TEST_CASE("Time GET, buffer too small: -1", "[device_api]")
{
    const timesync_status_t st = synced_from_dhcp();
    char buf[64];

    TEST_ASSERT_EQUAL(-1, device_api_time_json(buf, sizeof(buf), &st));
}

TEST_CASE("Time GET, the longest body: fits DEVICE_API_TIME_JSON_MAX", "[device_api]")
{
    timesync_status_t st = synced_from_dhcp();
    char buf[DEVICE_API_TIME_JSON_MAX];

    st.config.mode = TIMESYNC_MODE_CUSTOM;
    memset(st.config.server, 'a', TIMESYNC_SERVER_LEN - 1);
    st.config.server[TIMESYNC_SERVER_LEN - 1] = '\0';
    st.state = TIMESYNC_STATE_UNREACHABLE;
    st.now = INT64_MAX;
    st.synced_age_s = INT64_MAX;
    st.servers.count = 2;
    memset(st.servers.addr[0], 'b', TIMESYNC_SERVER_LEN - 1);
    st.servers.addr[0][TIMESYNC_SERVER_LEN - 1] = '\0';
    memset(st.servers.addr[1], 'c', TIMESYNC_SERVER_LEN - 1);
    st.servers.addr[1][TIMESYNC_SERVER_LEN - 1] = '\0';
    strcpy(st.dhcp.addr[0], "255.255.255.255");
    strcpy(st.dhcp.addr[1], "255.255.255.255");
    st.dhcp.iface = TIMESYNC_IFACE_THREAD_BB;
    TEST_ASSERT_GREATER_THAN(0, device_api_time_json(buf, sizeof(buf), &st));
}

/* ---- device_api_time_parse ---- */

TEST_CASE("Time PUT parse, a valid body: fills the config", "[device_api]")
{
    timesync_config_t out;
    const char *field = NULL;

    TEST_ASSERT_EQUAL(ESP_OK, device_api_time_parse("{\"enabled\":true,\"mode\":\"custom\",\"server\":\"ntp.home.lan\"}",
                      &out, &field));
    TEST_ASSERT_TRUE(out.enabled);
    TEST_ASSERT_EQUAL(TIMESYNC_MODE_CUSTOM, out.mode);
    TEST_ASSERT_EQUAL_STRING("ntp.home.lan", out.server);
}

TEST_CASE("Time PUT parse, a missing or wrongly typed key: names that field", "[device_api]")
{
    assert_parse_rejected("{\"enabled\":true,\"mode\":\"auto\"}", "server");
    assert_parse_rejected("{\"enabled\":1,\"mode\":\"auto\",\"server\":\"\"}", "enabled");
    assert_parse_rejected("{\"enabled\":true,\"server\":\"\"}", "mode");
}

TEST_CASE("Time PUT parse, an unknown mode: names mode", "[device_api]")
{
    assert_parse_rejected("{\"enabled\":true,\"mode\":\"manual\",\"server\":\"\"}", "mode");
}

TEST_CASE("Time PUT parse, not a JSON object: names body", "[device_api]")
{
    assert_parse_rejected("[]", "body");
    assert_parse_rejected("not json", "body");
}

TEST_CASE("Time PUT parse, a server of 64 bytes: names server", "[device_api]")
{
    char body[160];
    char server[65];

    memset(server, 'a', 64);
    server[64] = '\0';
    snprintf(body, sizeof(body), "{\"enabled\":true,\"mode\":\"custom\",\"server\":\"%s\"}", server);
    assert_parse_rejected(body, "server");
}
