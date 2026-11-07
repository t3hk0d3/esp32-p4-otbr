#include <string.h>

#include "radio_mux_core.h"
#include "unity.h"

static radio_mux_proxy_settings_t defaults(void)
{
    radio_mux_proxy_settings_t s;

    radio_mux_proxy_defaults(&s);
    return s;
}

static radio_mux_proxy_settings_t connect_to(const char *host)
{
    radio_mux_proxy_settings_t s = defaults();

    s.dir = RADIO_MUX_PROXY_CONNECT;
    strncpy(s.host, host, sizeof(s.host) - 1);
    return s;
}

/* ---- radio_mux_proxy_check (moved from radio_mux_proxy_check) ---- */

TEST_CASE("On the defaults (listen, tcp, 6638): returns RADIO_MUX_PROXY_CHECK_OK", "[radio_mux]")
{
    radio_mux_proxy_settings_t s = defaults();

    TEST_ASSERT_EQUAL(RADIO_MUX_PROXY_CHECK_OK, radio_mux_proxy_check(&s));
}

TEST_CASE("On connect without a host: returns RADIO_MUX_PROXY_CHECK_NEED_HOST", "[radio_mux]")
{
    radio_mux_proxy_settings_t s = connect_to("");

    TEST_ASSERT_EQUAL(RADIO_MUX_PROXY_CHECK_NEED_HOST, radio_mux_proxy_check(&s));
    s.adapter = RADIO_MUX_PROXY_UDP;
    TEST_ASSERT_EQUAL(RADIO_MUX_PROXY_CHECK_NEED_HOST, radio_mux_proxy_check(&s));
}

TEST_CASE("On listen without a host: returns RADIO_MUX_PROXY_CHECK_OK", "[radio_mux]")
{
    radio_mux_proxy_settings_t s = defaults();

    s.adapter = RADIO_MUX_PROXY_UDP;
    TEST_ASSERT_EQUAL(RADIO_MUX_PROXY_CHECK_OK, radio_mux_proxy_check(&s));
}

TEST_CASE("On framing with tcp: ignores it (kept for a later switch to udp)", "[radio_mux]")
{
    radio_mux_proxy_settings_t s = defaults();

    s.framing = RADIO_MUX_PROXY_RAW;
    TEST_ASSERT_EQUAL(RADIO_MUX_PROXY_CHECK_OK, radio_mux_proxy_check(&s));
    s.framing = RADIO_MUX_PROXY_HDLC;
    TEST_ASSERT_EQUAL(RADIO_MUX_PROXY_CHECK_OK, radio_mux_proxy_check(&s));
}

TEST_CASE("On port 80, 179 or 0: returns RADIO_MUX_PROXY_CHECK_BAD_PORT (above 65535 does not fit the uint16_t port)",
          "[radio_mux]")
{
    const uint16_t bad[] = {80, 179, 0};

    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        radio_mux_proxy_settings_t s = defaults();

        s.port = bad[i];
        TEST_ASSERT_EQUAL(RADIO_MUX_PROXY_CHECK_BAD_PORT, radio_mux_proxy_check(&s));
        s = connect_to("10.0.0.1");
        s.port = bad[i];
        TEST_ASSERT_EQUAL(RADIO_MUX_PROXY_CHECK_BAD_PORT, radio_mux_proxy_check(&s));
    }
}

TEST_CASE("On port 1 and port 65535: returns RADIO_MUX_PROXY_CHECK_OK", "[radio_mux]")
{
    radio_mux_proxy_settings_t s = defaults();

    s.port = 1;
    TEST_ASSERT_EQUAL(RADIO_MUX_PROXY_CHECK_OK, radio_mux_proxy_check(&s));
    s.port = 65535;
    TEST_ASSERT_EQUAL(RADIO_MUX_PROXY_CHECK_OK, radio_mux_proxy_check(&s));
}

TEST_CASE("On a bad port and no host for connect: returns RADIO_MUX_PROXY_CHECK_BAD_PORT first", "[radio_mux]")
{
    radio_mux_proxy_settings_t s = connect_to("");

    s.port = 80;
    TEST_ASSERT_EQUAL(RADIO_MUX_PROXY_CHECK_BAD_PORT, radio_mux_proxy_check(&s));
}

TEST_CASE("On an IPv4 or IPv6 literal host: returns RADIO_MUX_PROXY_CHECK_OK", "[radio_mux]")
{
    const char *good[] = {"192.168.1.10", "0.0.0.0", "255.255.255.255", "fd00::1", "::1", "::",
                          "2001:db8:0:0:1:0:0:1", "fe80::1:2:3:4", "::ffff:192.168.1.10", "2001:DB8::A"
                         };

    for (size_t i = 0; i < sizeof(good) / sizeof(good[0]); i++) {
        radio_mux_proxy_settings_t s = connect_to(good[i]);

        TEST_ASSERT_EQUAL_MESSAGE(RADIO_MUX_PROXY_CHECK_OK, radio_mux_proxy_check(&s), good[i]);
    }
}

TEST_CASE("On a DNS or .local name host: returns RADIO_MUX_PROXY_CHECK_OK", "[radio_mux]")
{
    const char *good[] = {"otbr-host.local", "pi.LOCAL", "agent.example.com", "a", "x-1.y2",
                          // 63 characters, the longest that fits
                          "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.example"
                         };

    for (size_t i = 0; i < sizeof(good) / sizeof(good[0]); i++) {
        radio_mux_proxy_settings_t s = connect_to(good[i]);

        TEST_ASSERT_EQUAL_MESSAGE(RADIO_MUX_PROXY_CHECK_OK, radio_mux_proxy_check(&s), good[i]);
    }
}

TEST_CASE("On a host that is neither a literal nor a valid name: returns RADIO_MUX_PROXY_CHECK_BAD_HOST", "[radio_mux]")
{
    const char *bad[] = {"bad host", "a..b", ".a", "a.", "-a.com", "a-.com", "under_score.local", "1.2.3.256",
                         "1.2.3", "fd00:::1", "1::2::3", "fd00::1%eth0", "[fd00::1]", "host:6638", "1:2:3:4:5:6:7:8:9",
                         "12345::1", "a/b"
                        };

    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        radio_mux_proxy_settings_t s = connect_to(bad[i]);

        TEST_ASSERT_EQUAL_MESSAGE(RADIO_MUX_PROXY_CHECK_BAD_HOST, radio_mux_proxy_check(&s), bad[i]);
    }
}

TEST_CASE("On a host that fills the buffer without a terminator: returns RADIO_MUX_PROXY_CHECK_BAD_HOST", "[radio_mux]")
{
    radio_mux_proxy_settings_t s = connect_to("");

    memset(s.host, 'a', sizeof(s.host));
    TEST_ASSERT_EQUAL(RADIO_MUX_PROXY_CHECK_BAD_HOST, radio_mux_proxy_check(&s));
}

TEST_CASE("On a bad host in listen mode: returns RADIO_MUX_PROXY_CHECK_BAD_HOST (a kept host stays valid)", "[radio_mux]")
{
    radio_mux_proxy_settings_t s = defaults();

    strcpy(s.host, "bad host");
    TEST_ASSERT_EQUAL(RADIO_MUX_PROXY_CHECK_BAD_HOST, radio_mux_proxy_check(&s));
}

TEST_CASE("On an unknown direction, adapter or framing: returns RADIO_MUX_PROXY_CHECK_BAD_KIND", "[radio_mux]")
{
    radio_mux_proxy_settings_t s = defaults();

    s.dir = (radio_mux_proxy_dir_t)7;
    TEST_ASSERT_EQUAL(RADIO_MUX_PROXY_CHECK_BAD_KIND, radio_mux_proxy_check(&s));
    s = defaults();
    s.adapter = (radio_mux_proxy_adapter_t)7;
    TEST_ASSERT_EQUAL(RADIO_MUX_PROXY_CHECK_BAD_KIND, radio_mux_proxy_check(&s));
    s = defaults();
    s.framing = (radio_mux_proxy_framing_t)7;
    TEST_ASSERT_EQUAL(RADIO_MUX_PROXY_CHECK_BAD_KIND, radio_mux_proxy_check(&s));
}

/* ---- radio_mux_proxy_merge ---- */

TEST_CASE("On no field given: returns OK with the stored settings, even stored ones the check refuses", "[radio_mux]")
{
    radio_mux_proxy_settings_t stored = defaults();
    radio_mux_proxy_settings_t given = connect_to("bad host");
    radio_mux_proxy_settings_t out;

    stored.port = 80; // stored before the rule existed: not the change's fault
    TEST_ASSERT_EQUAL(RADIO_MUX_PROXY_CHECK_OK, radio_mux_proxy_merge(&stored, 0, &given, &out));
    TEST_ASSERT_EQUAL_MEMORY(&stored, &out, sizeof(out));
}

TEST_CASE("On some fields given: only those replace the stored ones", "[radio_mux]")
{
    const radio_mux_proxy_settings_t stored = connect_to("10.0.0.1");
    radio_mux_proxy_settings_t given = defaults();
    radio_mux_proxy_settings_t out;

    given.port = 7000;
    given.adapter = RADIO_MUX_PROXY_UDP;
    given.dir = RADIO_MUX_PROXY_LISTEN; // not named: stays connect
    TEST_ASSERT_EQUAL(RADIO_MUX_PROXY_CHECK_OK, radio_mux_proxy_merge(&stored, RADIO_MUX_PROXY_F_PORT |
                      RADIO_MUX_PROXY_F_ADAPTER, &given, &out));
    TEST_ASSERT_EQUAL(7000, out.port);
    TEST_ASSERT_EQUAL(RADIO_MUX_PROXY_UDP, out.adapter);
    TEST_ASSERT_EQUAL(RADIO_MUX_PROXY_CONNECT, out.dir);
    TEST_ASSERT_EQUAL_STRING("10.0.0.1", out.host);
}

TEST_CASE("On connect given with the host stored earlier: returns OK", "[radio_mux]")
{
    radio_mux_proxy_settings_t stored = defaults();
    const radio_mux_proxy_settings_t given = connect_to("");
    radio_mux_proxy_settings_t out;

    strcpy(stored.host, "192.168.1.20");
    TEST_ASSERT_EQUAL(RADIO_MUX_PROXY_CHECK_OK, radio_mux_proxy_merge(&stored, RADIO_MUX_PROXY_F_DIR, &given, &out));
}

TEST_CASE("On merged settings the check refuses: returns its verdict", "[radio_mux]")
{
    const radio_mux_proxy_settings_t stored = defaults();
    radio_mux_proxy_settings_t given = connect_to("");
    radio_mux_proxy_settings_t out;

    TEST_ASSERT_EQUAL(RADIO_MUX_PROXY_CHECK_NEED_HOST, radio_mux_proxy_merge(&stored, RADIO_MUX_PROXY_F_DIR, &given,
                      &out));
    given.port = 179;
    TEST_ASSERT_EQUAL(RADIO_MUX_PROXY_CHECK_BAD_PORT, radio_mux_proxy_merge(&stored, RADIO_MUX_PROXY_F_PORT, &given,
                      &out));
    strcpy(given.host, "a..b");
    TEST_ASSERT_EQUAL(RADIO_MUX_PROXY_CHECK_BAD_HOST, radio_mux_proxy_merge(&stored, RADIO_MUX_PROXY_F_HOST, &given,
                      &out));
}
