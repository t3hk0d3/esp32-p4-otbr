#include <stdlib.h>
#include <string.h>
#include "unity.h"
#include "unity_test_runner.h"
#include "netnames_rules.h"

#define DEFAULT_HOST "esp-otbr-a1b2c3"

static netnames_settings_t blank(void)
{
    netnames_settings_t s = {.ha_name_follows_hostname = true};

    return s;
}

static bool check(const netnames_settings_t *s, const char **field)
{
    const char *reason = NULL;
    const bool ok = netnames_rules_check(s, field, &reason);

    if (!ok) {
        TEST_ASSERT_NOT_NULL(reason);
    }
    return ok;
}

static void assert_hostname_rejected(const char *name)
{
    netnames_settings_t s = blank();
    const char *field = NULL;

    // A name of 64 or more fills the buffer with no terminator: how an over-long name arrives from a caller.
    strncpy(s.hostname, name, sizeof(s.hostname));
    TEST_ASSERT_FALSE_MESSAGE(check(&s, &field), name);
    TEST_ASSERT_EQUAL_STRING("hostname", field);
}

/* ---- netnames_rules_check ---- */

TEST_CASE("All fields empty, follows hostname: valid", "[netnames]")
{
    netnames_settings_t s = blank();
    const char *field = NULL;

    TEST_ASSERT_TRUE(check(&s, &field));
}

TEST_CASE("Hostname in mixed case: valid", "[netnames]")
{
    netnames_settings_t s = blank();
    const char *field = NULL;

    strlcpy(s.hostname, "Kitchen-OTBR", sizeof(s.hostname));
    TEST_ASSERT_TRUE(check(&s, &field));
}

TEST_CASE("Hostname with a leading or trailing hyphen, a dot, an underscore, or 64 chars: rejected as hostname", "[netnames]")
{
    char long_name[65];

    memset(long_name, 'a', 64);
    long_name[64] = '\0';
    assert_hostname_rejected("-a");
    assert_hostname_rejected("a-");
    assert_hostname_rejected("a.b");
    assert_hostname_rejected("a_b");
    assert_hostname_rejected("a b");
    assert_hostname_rejected(long_name);
}

TEST_CASE("Hostname of 63 chars of a-z0-9-: valid", "[netnames]")
{
    netnames_settings_t s = blank();
    const char *field = NULL;

    memset(s.hostname, 'a', 63);
    s.hostname[30] = '-';
    s.hostname[40] = '7';
    s.hostname[63] = '\0';
    TEST_ASSERT_TRUE(check(&s, &field));
}

TEST_CASE("DHCP name invalid like a hostname: rejected as dhcp_name", "[netnames]")
{
    netnames_settings_t s = blank();
    const char *field = NULL;

    strlcpy(s.dhcp_name, "bad.name", sizeof(s.dhcp_name));
    TEST_ASSERT_FALSE(check(&s, &field));
    TEST_ASSERT_EQUAL_STRING("dhcp_name", field);
}

TEST_CASE("DHCP name of 32 chars: valid (the esp_netif hostname limit)", "[netnames]")
{
    netnames_settings_t s = blank();
    const char *field = NULL;

    memset(s.dhcp_name, 'd', 32);
    s.dhcp_name[32] = '\0';
    TEST_ASSERT_TRUE(check(&s, &field));
}

TEST_CASE("DHCP name of 33 chars: rejected as dhcp_name", "[netnames]")
{
    netnames_settings_t s = blank();
    const char *field = NULL;
    const char *reason = NULL;

    memset(s.dhcp_name, 'd', 33);
    s.dhcp_name[33] = '\0';
    TEST_ASSERT_FALSE(netnames_rules_check(&s, &field, &reason));
    TEST_ASSERT_EQUAL_STRING("dhcp_name", field);
    TEST_ASSERT_EQUAL_STRING("longer than 32 characters", reason);
}

TEST_CASE("HA name not following and empty: rejected as ha_name", "[netnames]")
{
    netnames_settings_t s = blank();
    const char *field = NULL;

    s.ha_name_follows_hostname = false;
    TEST_ASSERT_FALSE(check(&s, &field));
    TEST_ASSERT_EQUAL_STRING("ha_name", field);
}

TEST_CASE("HA name with a control character, a leading or a trailing space: rejected as ha_name", "[netnames]")
{
    static const char *const bad[] = {"Kitchen\tOTBR", "Kitchen\nOTBR", " Kitchen", "Kitchen ", "Kitchen\x7f"};

    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        netnames_settings_t s = blank();
        const char *field = NULL;

        s.ha_name_follows_hostname = false;
        strlcpy(s.ha_name, bad[i], sizeof(s.ha_name));
        TEST_ASSERT_FALSE_MESSAGE(check(&s, &field), bad[i]);
        TEST_ASSERT_EQUAL_STRING("ha_name", field);
    }
}

TEST_CASE("HA name of 63 bytes of UTF-8 (31 Cyrillic letters and one ASCII): valid", "[netnames]")
{
    netnames_settings_t s = blank();
    const char *field = NULL;

    s.ha_name_follows_hostname = false;
    for (int i = 0; i < 31; i++) {
        memcpy(s.ha_name + 2 * i, "\xd0\x96", 2); // Ж
    }
    s.ha_name[62] = 'x';
    s.ha_name[63] = '\0';
    TEST_ASSERT_EQUAL(63, strlen(s.ha_name));
    TEST_ASSERT_TRUE(check(&s, &field));
}

TEST_CASE("HA name of 64 bytes of UTF-8: rejected as ha_name", "[netnames]")
{
    char name[65];
    netnames_settings_t s = blank();
    const char *field = NULL;
    const char *reason = NULL;

    for (int i = 0; i < 32; i++) {
        memcpy(name + 2 * i, "\xd0\x96", 2);
    }
    name[64] = '\0';
    s.ha_name_follows_hostname = false;
    // 64 bytes fill the buffer with no terminator: how an over-long name arrives from a caller.
    memcpy(s.ha_name, name, sizeof(s.ha_name));
    TEST_ASSERT_FALSE(netnames_rules_check(&s, &field, &reason));
    TEST_ASSERT_EQUAL_STRING("ha_name", field);
}

TEST_CASE("HA name with invalid UTF-8: rejected as ha_name", "[netnames]")
{
    netnames_settings_t s = blank();
    const char *field = NULL;

    s.ha_name_follows_hostname = false;
    strlcpy(s.ha_name, "Kitchen \xd0", sizeof(s.ha_name));
    TEST_ASSERT_FALSE(check(&s, &field));
    TEST_ASSERT_EQUAL_STRING("ha_name", field);
}

TEST_CASE("HA name following the hostname but holding junk: valid (ignored)", "[netnames]")
{
    netnames_settings_t s = blank();
    const char *field = NULL;

    strlcpy(s.ha_name, " \t junk", sizeof(s.ha_name));
    TEST_ASSERT_TRUE(check(&s, &field));
}

/* ---- netnames_rules_normalise ---- */

TEST_CASE("Normalise, upper-case hostname and DHCP name: stored lower case, HA name untouched", "[netnames]")
{
    netnames_settings_t s = blank();

    strlcpy(s.hostname, "Kitchen-OTBR", sizeof(s.hostname));
    strlcpy(s.dhcp_name, "OTBR-1", sizeof(s.dhcp_name));
    strlcpy(s.ha_name, "Kitchen OTBR", sizeof(s.ha_name));
    netnames_rules_normalise(&s);
    TEST_ASSERT_EQUAL_STRING("kitchen-otbr", s.hostname);
    TEST_ASSERT_EQUAL_STRING("otbr-1", s.dhcp_name);
    TEST_ASSERT_EQUAL_STRING("Kitchen OTBR", s.ha_name);
}

/* ---- netnames_rules_default_hostname ---- */

TEST_CASE("Default hostname of MAC ..:a1:b2:c3: esp-otbr-a1b2c3", "[netnames]")
{
    const uint8_t mac[6] = {0x02, 0x11, 0x22, 0xa1, 0xb2, 0xc3};
    char out[NETNAMES_NAME_LEN];

    netnames_rules_default_hostname(mac, out);
    TEST_ASSERT_EQUAL_STRING("esp-otbr-a1b2c3", out);
}

/* ---- netnames_rules_effective ---- */

TEST_CASE("Effective, nothing set: default hostname, HA name = default hostname, DHCP name esp-otbr", "[netnames]")
{
    netnames_settings_t s = blank();
    char host[NETNAMES_NAME_LEN], ha[NETNAMES_NAME_LEN], dhcp[NETNAMES_NAME_LEN];

    netnames_rules_effective(&s, DEFAULT_HOST, host, ha, dhcp);
    TEST_ASSERT_EQUAL_STRING(DEFAULT_HOST, host);
    TEST_ASSERT_EQUAL_STRING(DEFAULT_HOST, ha);
    TEST_ASSERT_EQUAL_STRING("esp-otbr", dhcp);
}

TEST_CASE("Effective, hostname set and HA name follows: HA name = hostname", "[netnames]")
{
    netnames_settings_t s = blank();
    char host[NETNAMES_NAME_LEN], ha[NETNAMES_NAME_LEN], dhcp[NETNAMES_NAME_LEN];

    strlcpy(s.hostname, "kitchen-otbr", sizeof(s.hostname));
    strlcpy(s.dhcp_name, "router-name", sizeof(s.dhcp_name));
    netnames_rules_effective(&s, DEFAULT_HOST, host, ha, dhcp);
    TEST_ASSERT_EQUAL_STRING("kitchen-otbr", host);
    TEST_ASSERT_EQUAL_STRING("kitchen-otbr", ha);
    TEST_ASSERT_EQUAL_STRING("router-name", dhcp);
}

TEST_CASE("Effective, HA name not following: the set HA name", "[netnames]")
{
    netnames_settings_t s = blank();
    char host[NETNAMES_NAME_LEN], ha[NETNAMES_NAME_LEN], dhcp[NETNAMES_NAME_LEN];

    s.ha_name_follows_hostname = false;
    strlcpy(s.ha_name, "Kitchen OTBR", sizeof(s.ha_name));
    netnames_rules_effective(&s, DEFAULT_HOST, host, ha, dhcp);
    TEST_ASSERT_EQUAL_STRING(DEFAULT_HOST, host);
    TEST_ASSERT_EQUAL_STRING("Kitchen OTBR", ha);
}

/* ---- netnames_rules_steps ---- */

static void assert_steps(netnames_steps_t got, bool hostname, bool ha_name, bool http_advert, bool dhcp_name)
{
    TEST_ASSERT_EQUAL_MESSAGE(hostname, got.hostname, "hostname");
    TEST_ASSERT_EQUAL_MESSAGE(ha_name, got.ha_name, "ha_name");
    TEST_ASSERT_EQUAL_MESSAGE(http_advert, got.http_advert, "http_advert");
    TEST_ASSERT_EQUAL_MESSAGE(dhcp_name, got.dhcp_name, "dhcp_name");
}

TEST_CASE("Steps, nothing changed and live = effective hostname: none", "[netnames]")
{
    netnames_settings_t a = blank();

    strlcpy(a.hostname, "kitchen", sizeof(a.hostname));
    assert_steps(netnames_rules_steps(&a, &a, DEFAULT_HOST, "kitchen"), false, false, false, false);
}

TEST_CASE("Steps, only the DHCP name changed: dhcp_name only", "[netnames]")
{
    netnames_settings_t a = blank(), b = blank();

    strlcpy(b.dhcp_name, "router-name", sizeof(b.dhcp_name));
    assert_steps(netnames_rules_steps(&a, &b, DEFAULT_HOST, DEFAULT_HOST), false, false, false, true);
}

TEST_CASE("Steps, DHCP name set to its default value: none", "[netnames]")
{
    netnames_settings_t a = blank(), b = blank();

    strlcpy(b.dhcp_name, "esp-otbr", sizeof(b.dhcp_name));
    assert_steps(netnames_rules_steps(&a, &b, DEFAULT_HOST, DEFAULT_HOST), false, false, false, false);
}

TEST_CASE("Steps, hostname changed and HA name follows: hostname, ha_name and http_advert", "[netnames]")
{
    netnames_settings_t a = blank(), b = blank();

    strlcpy(b.hostname, "kitchen", sizeof(b.hostname));
    assert_steps(netnames_rules_steps(&a, &b, DEFAULT_HOST, DEFAULT_HOST), true, true, true, false);
}

TEST_CASE("Steps, hostname changed and HA name does not follow: hostname only", "[netnames]")
{
    netnames_settings_t a = blank(), b;

    a.ha_name_follows_hostname = false;
    strlcpy(a.ha_name, "Kitchen OTBR", sizeof(a.ha_name));
    b = a;
    strlcpy(b.hostname, "kitchen", sizeof(b.hostname));
    assert_steps(netnames_rules_steps(&a, &b, DEFAULT_HOST, DEFAULT_HOST), true, false, false, false);
}

TEST_CASE("Steps, hostname cleared to the default and HA name follows: hostname, ha_name and http_advert", "[netnames]")
{
    netnames_settings_t a = blank(), b = blank();

    strlcpy(a.hostname, "kitchen", sizeof(a.hostname));
    assert_steps(netnames_rules_steps(&a, &b, DEFAULT_HOST, "kitchen"), true, true, true, false);
}

TEST_CASE("Steps, same hostname but live differs (conflict rename): hostname", "[netnames]")
{
    netnames_settings_t a = blank();

    strlcpy(a.hostname, "kitchen", sizeof(a.hostname));
    assert_steps(netnames_rules_steps(&a, &a, DEFAULT_HOST, "kitchen-2"), true, false, false, false);
}

TEST_CASE("Steps, http_advert toggled: http_advert only", "[netnames]")
{
    netnames_settings_t a = blank(), b = blank();

    b.http_advert = true;
    assert_steps(netnames_rules_steps(&a, &b, DEFAULT_HOST, DEFAULT_HOST), false, false, true, false);
}

TEST_CASE("Steps, HA name stops following with the same text: none", "[netnames]")
{
    netnames_settings_t a = blank(), b = blank();

    b.ha_name_follows_hostname = false;
    strlcpy(b.ha_name, DEFAULT_HOST, sizeof(b.ha_name));
    assert_steps(netnames_rules_steps(&a, &b, DEFAULT_HOST, DEFAULT_HOST), false, false, false, false);
}

void app_main(void)
{
    UNITY_BEGIN();
    unity_run_all_tests();
    int failures = (int)UNITY_END();
    exit(failures == 0 ? 0 : 1);
}
