#include <stdlib.h>
#include <string.h>
#include "unity.h"
#include "unity_test_runner.h"
#include "timesync_rules.h"

#define S(x) ((int64_t)(x) * 1000000) // seconds to microseconds
#define NOW 1800000000LL              // 2027-01-15, a valid clock

static timesync_config_t config(bool enabled, timesync_mode_t mode, const char *server)
{
    timesync_config_t c = {.enabled = enabled, .mode = mode};

    // A server of 64 or more fills the buffer with no terminator: how an over-long one arrives from a caller.
    strncpy(c.server, server, sizeof(c.server));
    return c;
}

static timesync_dhcp_t lease(uint8_t count, timesync_iface_t iface)
{
    // 192.168.1.1, 192.168.1.2, 192.168.1.3 in network byte order (first octet first in memory).
    static const uint8_t bytes[3][4] = {{192, 168, 1, 1}, {192, 168, 1, 2}, {192, 168, 1, 3}};
    uint32_t addrs[3];
    timesync_dhcp_t d = {0};

    for (int i = 0; i < 3; i++) {
        memcpy(&addrs[i], bytes[i], 4);
    }
    timesync_rules_dhcp_capture(&d, count, addrs, iface);
    return d;
}

static void assert_valid(timesync_mode_t mode, const char *server)
{
    const timesync_config_t c = config(true, mode, server);
    const char *field = NULL, *reason = NULL;

    TEST_ASSERT_TRUE_MESSAGE(timesync_rules_check(&c, &field, &reason), server);
}

static void assert_rejected(timesync_mode_t mode, const char *server, const char *want_field)
{
    const timesync_config_t c = config(true, mode, server);
    const char *field = NULL, *reason = NULL;

    TEST_ASSERT_FALSE_MESSAGE(timesync_rules_check(&c, &field, &reason), server);
    TEST_ASSERT_EQUAL_STRING(want_field, field);
    TEST_ASSERT_NOT_NULL(reason);
}

/* ---- validate config ---- */

TEST_CASE("Auto with an empty server: accepts", "[timesync]")
{
    assert_valid(TIMESYNC_MODE_AUTO, "");
}

TEST_CASE("Custom with a hostname, an IPv4 literal or an IPv6 literal: accepts", "[timesync]")
{
    assert_valid(TIMESYNC_MODE_CUSTOM, "ntp.home.lan");
    assert_valid(TIMESYNC_MODE_CUSTOM, "time-1.example.com");
    assert_valid(TIMESYNC_MODE_CUSTOM, "192.168.1.1");
    assert_valid(TIMESYNC_MODE_CUSTOM, "2001:db8::1");
    assert_valid(TIMESYNC_MODE_CUSTOM, "fe80::1:2");
}

TEST_CASE("Custom with an empty server: rejects, naming server", "[timesync]")
{
    assert_rejected(TIMESYNC_MODE_CUSTOM, "", "server");
}

TEST_CASE("A server over 63 bytes, with a space, or a label with an edge hyphen: rejects, naming server", "[timesync]")
{
    char long_name[80];

    memset(long_name, 'a', 64);
    long_name[64] = '\0';
    assert_rejected(TIMESYNC_MODE_CUSTOM, long_name, "server");
    assert_rejected(TIMESYNC_MODE_CUSTOM, "a b", "server");
    assert_rejected(TIMESYNC_MODE_CUSTOM, "-bad.lan", "server");
    assert_rejected(TIMESYNC_MODE_CUSTOM, "bad-.lan", "server");
    assert_rejected(TIMESYNC_MODE_CUSTOM, "two..dots", "server");
    assert_rejected(TIMESYNC_MODE_CUSTOM, "trailing.", "server");
}

TEST_CASE("Auto with a server over 63 bytes: rejects, naming server", "[timesync]")
{
    char long_name[80];

    memset(long_name, 'a', 64);
    long_name[64] = '\0';
    assert_rejected(TIMESYNC_MODE_AUTO, long_name, "server");
}

TEST_CASE("An unknown mode: rejects, naming mode", "[timesync]")
{
    assert_rejected((timesync_mode_t)7, "ntp.home.lan", "mode");
}

/* ---- choose servers ---- */

TEST_CASE("Choose, disabled: no servers, source none", "[timesync]")
{
    const timesync_config_t c = config(false, TIMESYNC_MODE_AUTO, "");
    const timesync_dhcp_t d = lease(2, TIMESYNC_IFACE_SINGLE);
    timesync_servers_t out;

    timesync_rules_choose(&c, &d, &out);
    TEST_ASSERT_EQUAL(0, out.count);
    TEST_ASSERT_EQUAL(TIMESYNC_SOURCE_NONE, out.source);
}

TEST_CASE("Choose, custom whatever the lease names: the custom server alone", "[timesync]")
{
    const timesync_config_t c = config(true, TIMESYNC_MODE_CUSTOM, "ntp.home.lan");
    const timesync_dhcp_t d = lease(2, TIMESYNC_IFACE_SINGLE);
    timesync_servers_t out;

    timesync_rules_choose(&c, &d, &out);
    TEST_ASSERT_EQUAL(1, out.count);
    TEST_ASSERT_EQUAL_STRING("ntp.home.lan", out.addr[0]);
    TEST_ASSERT_EQUAL(TIMESYNC_SOURCE_CUSTOM, out.source);
}

TEST_CASE("Choose, auto with a lease naming one server: that server", "[timesync]")
{
    const timesync_config_t c = config(true, TIMESYNC_MODE_AUTO, "");
    const timesync_dhcp_t d = lease(1, TIMESYNC_IFACE_SINGLE);
    timesync_servers_t out;

    timesync_rules_choose(&c, &d, &out);
    TEST_ASSERT_EQUAL(1, out.count);
    TEST_ASSERT_EQUAL_STRING("192.168.1.1", out.addr[0]);
    TEST_ASSERT_EQUAL(TIMESYNC_SOURCE_DHCP, out.source);
}

TEST_CASE("Choose, auto with a lease naming three servers: the first two", "[timesync]")
{
    const timesync_config_t c = config(true, TIMESYNC_MODE_AUTO, "");
    const timesync_dhcp_t d = lease(3, TIMESYNC_IFACE_SINGLE);
    timesync_servers_t out;

    timesync_rules_choose(&c, &d, &out);
    TEST_ASSERT_EQUAL(2, out.count);
    TEST_ASSERT_EQUAL_STRING("192.168.1.1", out.addr[0]);
    TEST_ASSERT_EQUAL_STRING("192.168.1.2", out.addr[1]);
    TEST_ASSERT_EQUAL(TIMESYNC_SOURCE_DHCP, out.source);
}

TEST_CASE("Choose, auto with no server in the lease: pool.ntp.org", "[timesync]")
{
    const timesync_config_t c = config(true, TIMESYNC_MODE_AUTO, "ignored.lan");
    const timesync_dhcp_t d = {0};
    timesync_servers_t out;

    timesync_rules_choose(&c, &d, &out);
    TEST_ASSERT_EQUAL(1, out.count);
    TEST_ASSERT_EQUAL_STRING("pool.ntp.org", out.addr[0]);
    TEST_ASSERT_EQUAL(TIMESYNC_SOURCE_DEFAULT, out.source);
}

/* ---- servers unchanged (a renewal naming the same servers restarts nothing) ---- */

TEST_CASE("Servers, same list and source: equal", "[timesync]")
{
    const timesync_config_t c = config(true, TIMESYNC_MODE_AUTO, "");
    const timesync_dhcp_t d = lease(2, TIMESYNC_IFACE_MGMT);
    timesync_servers_t a, b;

    timesync_rules_choose(&c, &d, &a);
    timesync_rules_choose(&c, &d, &b);
    TEST_ASSERT_TRUE(timesync_rules_servers_equal(&a, &b));
}

TEST_CASE("Servers, other order or other source: not equal", "[timesync]")
{
    timesync_servers_t a = {.count = 2, .addr = {"a", "b"}, .source = TIMESYNC_SOURCE_DHCP};
    timesync_servers_t b = {.count = 2, .addr = {"b", "a"}, .source = TIMESYNC_SOURCE_DHCP};
    timesync_servers_t c = {.count = 2, .addr = {"a", "b"}, .source = TIMESYNC_SOURCE_CUSTOM};

    TEST_ASSERT_FALSE(timesync_rules_servers_equal(&a, &b));
    TEST_ASSERT_FALSE(timesync_rules_servers_equal(&a, &c));
}

/* ---- derive state ---- */

TEST_CASE("State, disabled: off", "[timesync]")
{
    TEST_ASSERT_EQUAL(TIMESYNC_STATE_OFF, timesync_rules_state(false, S(600), true, true, S(60)));
}

TEST_CASE("State, enabled, no answer, 59 s: waiting", "[timesync]")
{
    TEST_ASSERT_EQUAL(TIMESYNC_STATE_WAITING, timesync_rules_state(true, S(59), false, false, -1));
}

TEST_CASE("State, enabled, no answer, 60 s: unreachable", "[timesync]")
{
    TEST_ASSERT_EQUAL(TIMESYNC_STATE_UNREACHABLE, timesync_rules_state(true, S(60), false, false, -1));
}

TEST_CASE("State, answered and a server reachable: synced", "[timesync]")
{
    TEST_ASSERT_EQUAL(TIMESYNC_STATE_SYNCED, timesync_rules_state(true, S(5000), true, true, S(60)));
}

TEST_CASE("State, answered earlier and every server now unreachable: unreachable", "[timesync]")
{
    TEST_ASSERT_EQUAL(TIMESYNC_STATE_UNREACHABLE, timesync_rules_state(true, S(5000), true, false, S(60)));
}

TEST_CASE("State, answered 3 h ago, register still non-zero (DNS lost): unreachable", "[timesync]")
{
    // lwIP shifts the reachability register only when it sends; a hostname that stops resolving sends nothing.
    TEST_ASSERT_EQUAL(TIMESYNC_STATE_UNREACHABLE, timesync_rules_state(true, S(20000), true, true, S(3 * 3600)));
}

TEST_CASE("State, answered 1 h ago and reachable: synced", "[timesync]")
{
    TEST_ASSERT_EQUAL(TIMESYNC_STATE_SYNCED, timesync_rules_state(true, S(20000), true, true, S(3600)));
}

/* ---- epoch at, age, clock valid ---- */

TEST_CASE("Epoch at, valid clock, 90 s ago: now minus 90", "[timesync]")
{
    TEST_ASSERT_EQUAL_INT64(NOW - 90, timesync_rules_epoch_at(NOW, S(100), S(10)));
}

TEST_CASE("Epoch at, a moment before the first sync: its real date", "[timesync]")
{
    // Boot +5 s, the clock was stepped from 1970 at +30 s; read at +600 s.
    TEST_ASSERT_EQUAL_INT64(NOW - 595, timesync_rules_epoch_at(NOW, S(600), S(5)));
}

TEST_CASE("Epoch at, clock not valid: 0", "[timesync]")
{
    TEST_ASSERT_EQUAL_INT64(0, timesync_rules_epoch_at(1000, S(100), S(10)));
}

TEST_CASE("Age, synced 240 s ago: 240; never: -1", "[timesync]")
{
    TEST_ASSERT_EQUAL_INT64(240, timesync_rules_age_s(S(300), S(60)));
    TEST_ASSERT_EQUAL_INT64(-1, timesync_rules_age_s(S(300), -1));
}

TEST_CASE("Clock valid: false before 2020-01-01, true from it", "[timesync]")
{
    TEST_ASSERT_FALSE(timesync_rules_clock_valid(1577836799));
    TEST_ASSERT_TRUE(timesync_rules_clock_valid(1577836800));
}

/* ---- DHCP capture ---- */

TEST_CASE("Capture, a mgmt lease then a thread_bb lease: the second wins whole", "[timesync]")
{
    static const uint8_t first[2][4] = {{10, 0, 10, 1}, {10, 0, 10, 2}};
    static const uint8_t second[1][4] = {{10, 0, 20, 1}};
    uint32_t a[2], b[1];
    timesync_dhcp_t d = {0};

    memcpy(&a[0], first[0], 4);
    memcpy(&a[1], first[1], 4);
    memcpy(&b[0], second[0], 4);
    timesync_rules_dhcp_capture(&d, 2, a, TIMESYNC_IFACE_MGMT);
    timesync_rules_dhcp_capture(&d, 1, b, TIMESYNC_IFACE_THREAD_BB);
    TEST_ASSERT_EQUAL(1, d.count);
    TEST_ASSERT_EQUAL_STRING("10.0.20.1", d.addr[0]);
    TEST_ASSERT_EQUAL(TIMESYNC_IFACE_THREAD_BB, d.iface);
}

TEST_CASE("Capture with count 0: clears, interface included", "[timesync]")
{
    timesync_dhcp_t d = lease(2, TIMESYNC_IFACE_MGMT);

    timesync_rules_dhcp_capture(&d, 0, NULL, TIMESYNC_IFACE_MGMT);
    TEST_ASSERT_EQUAL(0, d.count);
    TEST_ASSERT_EQUAL(TIMESYNC_IFACE_NONE, d.iface);
}

TEST_CASE("Capture with count 0 from another interface: keeps what the first lease named", "[timesync]")
{
    timesync_dhcp_t d = lease(2, TIMESYNC_IFACE_MGMT);

    timesync_rules_dhcp_capture(&d, 0, NULL, TIMESYNC_IFACE_THREAD_BB);
    TEST_ASSERT_EQUAL(2, d.count);
    TEST_ASSERT_EQUAL(TIMESYNC_IFACE_MGMT, d.iface);
}

/* ---- API names ---- */

TEST_CASE("Names: the spec 5.2 strings, NULL for none", "[timesync]")
{
    TEST_ASSERT_EQUAL_STRING("auto", timesync_mode_name(TIMESYNC_MODE_AUTO));
    TEST_ASSERT_EQUAL_STRING("custom", timesync_mode_name(TIMESYNC_MODE_CUSTOM));
    TEST_ASSERT_EQUAL_STRING("off", timesync_state_name(TIMESYNC_STATE_OFF));
    TEST_ASSERT_EQUAL_STRING("waiting", timesync_state_name(TIMESYNC_STATE_WAITING));
    TEST_ASSERT_EQUAL_STRING("synced", timesync_state_name(TIMESYNC_STATE_SYNCED));
    TEST_ASSERT_EQUAL_STRING("unreachable", timesync_state_name(TIMESYNC_STATE_UNREACHABLE));
    TEST_ASSERT_EQUAL_STRING("custom", timesync_source_name(TIMESYNC_SOURCE_CUSTOM));
    TEST_ASSERT_EQUAL_STRING("dhcp", timesync_source_name(TIMESYNC_SOURCE_DHCP));
    TEST_ASSERT_EQUAL_STRING("default", timesync_source_name(TIMESYNC_SOURCE_DEFAULT));
    TEST_ASSERT_NULL(timesync_source_name(TIMESYNC_SOURCE_NONE));
    TEST_ASSERT_EQUAL_STRING("single", timesync_iface_name(TIMESYNC_IFACE_SINGLE));
    TEST_ASSERT_EQUAL_STRING("mgmt", timesync_iface_name(TIMESYNC_IFACE_MGMT));
    TEST_ASSERT_EQUAL_STRING("thread_bb", timesync_iface_name(TIMESYNC_IFACE_THREAD_BB));
    TEST_ASSERT_NULL(timesync_iface_name(TIMESYNC_IFACE_NONE));
}

void app_main(void)
{
    UNITY_BEGIN();
    unity_run_all_tests();
    int failures = (int)UNITY_END();
    exit(failures == 0 ? 0 : 1);
}
