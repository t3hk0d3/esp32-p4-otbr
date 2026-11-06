#include <stdio.h>
#include <string.h>
#include "unity.h"
#include "unity_test_runner.h"
#include "br_core_mode.h"
#include "device_api_priv.h"
#include "routing_types.h"

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
        .otbr_channel = "c6",
    };
    strcpy(s.ipv6[0], "fe80::1");
    strcpy(s.ipv6[1], "fd00::2");
    s.ipv6_count = 2;
    return s;
}

static char body[2048];

static void render(const device_api_status_t *s)
{
    TEST_ASSERT_GREATER_THAN(0, device_api_status_json(body, sizeof(body), s));
}

TEST_CASE("On any mode: the mode name is written", "[device_api]")
{
    const char *names[] = {"STARTING", "BR_RUNNING", "SNIFFER", "RADIO_FAULT", "NO_RADIO"};

    for (int m = 0; m < 5; m++) {
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

TEST_CASE("On a build without TREL: trel is null", "[device_api]")
{
    device_api_status_t s = sample();

    render(&s);
    TEST_ASSERT_NOT_NULL(strstr(body, "\"trel\":null,\"otbr_channel\":\"c6\",\"zigbee_channel\""));
}

TEST_CASE("On a board in NO_RADIO: mode NO_RADIO, otbr_channel as stored, right after trel", "[device_api]")
{
    device_api_status_t s = sample();

    s.mode = BR_MODE_NO_RADIO;
    s.otbr_channel = "usb-10c4-ea60-0001";
    s.fault_reason = "radio unplugged (usb-10c4-ea60-0001)";
    render(&s);
    TEST_ASSERT_NOT_NULL(strstr(body, "\"mode\":\"NO_RADIO\""));
    TEST_ASSERT_NOT_NULL(strstr(body, "\"trel\":null,\"otbr_channel\":\"usb-10c4-ea60-0001\","));
    TEST_ASSERT_NOT_NULL(strstr(body, "\"fault_reason\":\"radio unplugged (usb-10c4-ea60-0001)\""));
}

TEST_CASE("On no channel stored in otbr: otbr_channel is null", "[device_api]")
{
    device_api_status_t s = sample();

    s.mode = BR_MODE_NO_RADIO;
    s.otbr_channel = "";
    render(&s);
    TEST_ASSERT_NOT_NULL(strstr(body, "\"otbr_channel\":null,"));
    s.otbr_channel = NULL;
    render(&s);
    TEST_ASSERT_NOT_NULL(strstr(body, "\"otbr_channel\":null,"));
}

TEST_CASE("On TREL whose state could not be read: enabled is written, the rest is null", "[device_api]")
{
    device_api_status_t s = sample();

    s.trel.built = true;
    s.trel.enabled = true;
    render(&s);
    TEST_ASSERT_NOT_NULL(strstr(body, "\"trel\":{\"enabled\":true,\"active\":null,\"peers\":null,\"peers_other\":null,"
                                "\"tx_packets\":null,\"tx_failures\":null,\"rx_packets\":null,\"peer_list\":[]}"));
}

/* Two peers: a neighbour of this network (both radios) and a peer of another network (no preference). */
static device_api_status_t trel_two_networks(void)
{
    device_api_status_t s = sample();

    s.trel = (device_api_trel_t) {
        .built = true, .enabled = true, .state_valid = true, .active = true, .peers = 1, .peers_other = 1,
        .tx_packets = 5000000000ULL, .tx_failures = 1, .rx_packets = 17, .peer_count = 2,
    };
    s.trel.peer_list[0] = (device_api_trel_peer_t) {
        .same_network = true, .neighbor = true, .pref_154 = 230, .pref_trel = 55,
    };
    strcpy(s.trel.peer_list[0].ext_address, "1a2b3c4d5e6f7081");
    strcpy(s.trel.peer_list[0].ext_pan_id, "adf0d025c4b51f00");
    strcpy(s.trel.peer_list[0].address, "[fe80::1]:12390");
    s.trel.peer_list[1] = (device_api_trel_peer_t) {
        .pref_154 = DEVICE_API_TREL_PREF_NONE, .pref_trel = DEVICE_API_TREL_PREF_NONE,
    };
    strcpy(s.trel.peer_list[1].ext_address, "9f8e7d6c5b4a3921");
    strcpy(s.trel.peer_list[1].ext_pan_id, "5965b57e30c0def4");
    strcpy(s.trel.peer_list[1].address, "[fe80::2]:12390");
    return s;
}

TEST_CASE("On TREL peers of both networks: state, counters and both peer counts are written", "[device_api]")
{
    device_api_status_t s = trel_two_networks();

    render(&s);
    TEST_ASSERT_NOT_NULL(strstr(body, "\"trel\":{\"enabled\":true,\"active\":true,\"peers\":1,\"peers_other\":1,"
                                "\"tx_packets\":5000000000,\"tx_failures\":1,\"rx_packets\":17,\"peer_list\":["));
}

TEST_CASE("On TREL peers of both networks: each entry has ext_pan_id, same_network and preference", "[device_api]")
{
    device_api_status_t s = trel_two_networks();

    render(&s);
    TEST_ASSERT_NOT_NULL(strstr(body, "\"peer_list\":[{\"ext_address\":\"1a2b3c4d5e6f7081\",\"ext_pan_id\":\"adf0d025c4b51f00\","
                                "\"same_network\":true,\"preference\":{\"ieee802154\":230,\"trel\":55},"
                                "\"address\":\"[fe80::1]:12390\"},"));
}

TEST_CASE("On a TREL peer that is not a neighbour: preference is null", "[device_api]")
{
    device_api_status_t s = trel_two_networks();

    render(&s);
    TEST_ASSERT_NOT_NULL(strstr(body, "{\"ext_address\":\"9f8e7d6c5b4a3921\",\"ext_pan_id\":\"5965b57e30c0def4\","
                                "\"same_network\":false,\"preference\":null,\"address\":\"[fe80::2]:12390\"}]}"));
}

TEST_CASE("On a TREL neighbour that never used 15.4: preference.ieee802154 is null", "[device_api]")
{
    device_api_status_t s = trel_two_networks();

    s.trel.peer_list[0].pref_154 = DEVICE_API_TREL_PREF_NONE;
    s.trel.peer_list[0].pref_trel = 255;
    render(&s);
    TEST_ASSERT_NOT_NULL(strstr(body, "\"preference\":{\"ieee802154\":null,\"trel\":255}"));
}

TEST_CASE("On TREL switched off: enabled false, active false", "[device_api]")
{
    device_api_status_t s = sample();

    s.trel = (device_api_trel_t) {
        .built = true, .enabled = false, .state_valid = true, .active = false,
    };
    render(&s);
    TEST_ASSERT_NOT_NULL(strstr(body, "\"trel\":{\"enabled\":false,\"active\":false,\"peers\":0,"));
}

TEST_CASE("On a full status (8 TREL peers, 6 IPv6 addresses, BGP routing): fits the 3072-byte handler buffer", "[device_api]")
{
    device_api_status_t s = sample();
    char full[3072];

    for (size_t i = 0; i < DEVICE_API_MAX_IPV6; i++) {
        strcpy(s.ipv6[i], "2001:db8:1234:5678:9abc:def0:1234:5678");
    }
    s.ipv6_count = DEVICE_API_MAX_IPV6;
    s.routing_started = true;
    s.routing_method = ROUTING_METHOD_BGP;
    s.routing_omr_mode = ROUTING_OMR_CUSTOM;
    s.otbr_channel = "usb-ffff-ffff-0123456789abcdef0123456789abcdef0"; // 47 characters, the longest id
    s.routing_announced_count = UINT8_MAX;
    s.routing_peers_established = UINT8_MAX;
    s.routing_peers_configured = UINT8_MAX;
    s.trel = (device_api_trel_t) {
        .built = true, .enabled = true, .state_valid = true, .active = true, .peers = UINT16_MAX,
        .peers_other = UINT16_MAX, .peer_count = 8,
        .tx_packets = UINT64_MAX, .tx_failures = UINT64_MAX, .rx_packets = UINT64_MAX,
    };
    for (size_t i = 0; i < DEVICE_API_TREL_PEERS_MAX; i++) {
        s.trel.peer_list[i] = (device_api_trel_peer_t) {
            .same_network = false, .neighbor = true, .pref_154 = 255, .pref_trel = 255,
        };
        strcpy(s.trel.peer_list[i].ext_address, "1a2b3c4d5e6f7081");
        strcpy(s.trel.peer_list[i].ext_pan_id, "5965b57e30c0def4");
        strcpy(s.trel.peer_list[i].address, "[2001:db8:1234:5678:9abc:def0:1234:5678]:12390");
    }
    TEST_ASSERT_GREATER_THAN(0, device_api_status_json(full, sizeof(full), &s)); // 2505 bytes on 2026-11-03
}

TEST_CASE("routing bgp: renders method, counts and peers", "[routing]")
{
    device_api_status_t s = sample();

    s.routing_started = true;
    s.routing_method = ROUTING_METHOD_BGP;
    s.routing_omr_mode = ROUTING_OMR_CUSTOM;
    s.routing_announced_count = 2;
    s.routing_peers_established = 1;
    s.routing_peers_configured = 2;
    render(&s);
    TEST_ASSERT_NOT_NULL(strstr(body, "\"routing\":{\"method\":\"bgp\",\"omr_mode\":\"custom\",\"announced_count\":2,"
                                "\"peers_established\":1,\"peers_configured\":2}"));
}

TEST_CASE("routing ripng: peer fields are null", "[routing]")
{
    device_api_status_t s = sample();

    s.routing_started = true;
    s.routing_method = ROUTING_METHOD_RIPNG;
    s.routing_omr_mode = ROUTING_OMR_AUTO;
    s.routing_announced_count = 1;
    s.routing_peers_established = 1; // a stale figure: not written outside bgp
    s.routing_peers_configured = 2;
    render(&s);
    TEST_ASSERT_NOT_NULL(strstr(body, "\"routing\":{\"method\":\"ripng\",\"omr_mode\":\"auto\",\"announced_count\":1,"
                                "\"peers_established\":null,\"peers_configured\":null}"));
}

TEST_CASE("routing not started: routing is null", "[routing]")
{
    device_api_status_t s = sample();

    s.routing_started = false;
    render(&s);
    TEST_ASSERT_NOT_NULL(strstr(body, "\"routing\":null"));
    TEST_ASSERT_NOT_NULL(strstr(body, "\"pd\":{\"state\":\"running\",\"prefix\":\"fd00:1::/64\"}"));
}

TEST_CASE("On a channel running ziggurat: zigbee_channel is its id", "[device_api]")
{
    device_api_status_t s = sample();

    s.zigbee_channel = "usb-303a:1001";
    render(&s);
    TEST_ASSERT_NOT_NULL(strstr(body, "\"zigbee_channel\":\"usb-303a:1001\""));
}

TEST_CASE("On no channel running ziggurat: zigbee_channel is null", "[device_api]")
{
    device_api_status_t s = sample();

    render(&s);
    TEST_ASSERT_NOT_NULL(strstr(body, "\"zigbee_channel\":null"));
    s.zigbee_channel = "";
    render(&s);
    TEST_ASSERT_NOT_NULL(strstr(body, "\"zigbee_channel\":null"));
}
