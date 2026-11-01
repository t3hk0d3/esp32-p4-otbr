#include <stdio.h>
#include <string.h>
#include "unity.h"
#include "unity_test_runner.h"
#include "device_api_priv.h"

static device_api_status_t sample(void)
{
    device_api_status_t s = {
        .mode = 1,
        .app_version = "0.1.0",
        .app_slot = 1,
        .rcp_installed_known = true,
        .rcp_installed = "1.2.3",
        .rcp_compatible = true,
        .jumper_present = true,
        .base_expected = "b1",
        .base_installed = "b0",
        .ipv4 = "192.168.1.5",
        .pd_state = 2,
        .pd_prefix = "fd00:1::/64",
        .sniffer_active = false,
        .sniffer_channel = 15,
        .sniffer_frames = 7,
        .sniffer_drops = 1,
        .sniffer_elapsed_s = 3,
        .sniffer_duration_s = 600,
        .fault_reason = "",
        .storage_ok = true,
    };
    strcpy(s.ipv6[0], "fe80::1");
    strcpy(s.ipv6[1], "fd00::2");
    s.ipv6_count = 2;
    return s;
}

static char body[1024];

static void render(const device_api_status_t *s)
{
    TEST_ASSERT_GREATER_THAN(0, device_api_status_json(body, sizeof(body), s));
}

TEST_CASE("On any mode: the mode name is written", "[device_api]")
{
    const char *names[] = {"STARTING", "BR_RUNNING", "SNIFFER", "RADIO_FAULT"};

    for (int m = 0; m < 4; m++) {
        device_api_status_t s = sample();
        s.mode = m;
        render(&s);
        char want[48];
        snprintf(want, sizeof(want), "\"mode\":\"%s\"", names[m]);
        TEST_ASSERT_NOT_NULL(strstr(body, want));
    }
}

TEST_CASE("On a healthy status: versions and the slot are written", "[device_api]")
{
    device_api_status_t s = sample();

    render(&s);
    TEST_ASSERT_NOT_NULL(strstr(body, "\"app\":{\"version\":\"0.1.0\",\"slot\":1}"));
}

TEST_CASE("On an installed RCP: rcp has installed and compatible only", "[device_api]")
{
    device_api_status_t s = sample();

    render(&s);
    TEST_ASSERT_NOT_NULL(strstr(body, "\"rcp\":{\"installed\":\"1.2.3\",\"compatible\":true}"));
    TEST_ASSERT_NULL(strstr(body, "outdated"));
    TEST_ASSERT_NULL(strstr(body, "resync"));
}

TEST_CASE("On no valid RCP firmware: rcp.installed is an empty string", "[device_api]")
{
    device_api_status_t s = sample();

    s.rcp_installed = "";
    s.rcp_compatible = false;
    render(&s);
    TEST_ASSERT_NOT_NULL(strstr(body, "\"rcp\":{\"installed\":\"\",\"compatible\":false}"));
}

TEST_CASE("On a failed probe: rcp.installed is null", "[device_api]")
{
    device_api_status_t s = sample();

    s.rcp_installed_known = false;
    s.rcp_installed = "";
    render(&s);
    TEST_ASSERT_NOT_NULL(strstr(body, "\"rcp\":{\"installed\":null,"));
}

TEST_CASE("On the jumper: presence and the base versions are written", "[device_api]")
{
    device_api_status_t s = sample();

    render(&s);
    TEST_ASSERT_NOT_NULL(strstr(body, "\"jumper\":{\"present\":true,\"base_expected\":\"b1\",\"base_installed\":\"b0\",\"recovery_failed\":false}"));
}

TEST_CASE("On Ethernet addresses: the IPv4 address and the IPv6 list are written", "[device_api]")
{
    device_api_status_t s = sample();

    render(&s);
    TEST_ASSERT_NOT_NULL(strstr(body, "\"eth\":{\"ipv4\":\"192.168.1.5\",\"ipv6\":[\"fe80::1\",\"fd00::2\"]}"));
}

TEST_CASE("On no Ethernet address: ipv4 is null and ipv6 an empty list", "[device_api]")
{
    device_api_status_t s = sample();

    s.ipv4 = "";
    s.ipv6_count = 0;
    render(&s);
    TEST_ASSERT_NOT_NULL(strstr(body, "\"eth\":{\"ipv4\":null,\"ipv6\":[]}"));
}

TEST_CASE("On a PD state and prefix: the state name and the prefix are written", "[device_api]")
{
    device_api_status_t s = sample();

    render(&s);
    TEST_ASSERT_NOT_NULL(strstr(body, "\"pd\":{\"state\":\"running\",\"prefix\":\"fd00:1::/64\"}"));
    s.pd_state = -1;
    s.pd_prefix = "";
    render(&s);
    TEST_ASSERT_NOT_NULL(strstr(body, "\"pd\":{\"state\":\"uninitialized\",\"prefix\":null}"));
    s.pd_state = 0;
    render(&s);
    TEST_ASSERT_NOT_NULL(strstr(body, "\"state\":\"disabled\""));
}

TEST_CASE("On a PD state that could not be read (OpenThread lock busy): pd.state and pd.prefix are null", "[device_api]")
{
    device_api_status_t s = sample();

    s.pd_state = DEVICE_API_PD_STATE_UNKNOWN;
    s.pd_prefix = "";
    render(&s);
    TEST_ASSERT_NOT_NULL(strstr(body, "\"pd\":{\"state\":null,\"prefix\":null}"));
}

TEST_CASE("On sniffer stats: every field of the stats is written", "[device_api]")
{
    device_api_status_t s = sample();

    render(&s);
    TEST_ASSERT_NOT_NULL(strstr(body, "\"sniffer\":{\"active\":false,\"ended_by_fault\":false,\"channel\":15,\"frames\":7,"
                                "\"drops\":1,\"elapsed_s\":3,\"duration_s\":600}"));
}

TEST_CASE("On RADIO_FAULT: fault_reason carries the reason", "[device_api]")
{
    device_api_status_t s = sample();

    s.mode = 3;
    s.fault_reason = "no rcp firmware";
    render(&s);
    TEST_ASSERT_NOT_NULL(strstr(body, "\"fault_reason\":\"no rcp firmware\""));
    s.fault_reason = "rcp incompatible";
    render(&s);
    TEST_ASSERT_NOT_NULL(strstr(body, "\"fault_reason\":\"rcp incompatible\""));
}

TEST_CASE("On a healthy storage: storage_ok is true", "[device_api]")
{
    device_api_status_t s = sample();

    render(&s);
    TEST_ASSERT_NOT_NULL(strstr(body, "\"storage_ok\":true"));
}

TEST_CASE("On a degraded boot (NVS did not initialise): storage_ok is false", "[device_api]")
{
    device_api_status_t s = sample();

    s.storage_ok = false;
    render(&s);
    TEST_ASSERT_NOT_NULL(strstr(body, "\"storage_ok\":false"));
}

TEST_CASE("On no fault: fault_reason is null", "[device_api]")
{
    device_api_status_t s = sample();

    render(&s);
    TEST_ASSERT_NOT_NULL(strstr(body, "\"fault_reason\":null"));
}

TEST_CASE("On a buffer too small for the body: returns -1", "[device_api]")
{
    device_api_status_t s = sample();
    char small[64];

    TEST_ASSERT_EQUAL(-1, device_api_status_json(small, sizeof(small), &s));
}

TEST_CASE("On a failed ROM recovery: jumper.recovery_failed is true", "[device_api]")
{
    device_api_status_t s = sample();

    s.jumper_recovery_failed = true;
    render(&s);
    TEST_ASSERT_NOT_NULL(strstr(body, "\"recovery_failed\":true"));
}
