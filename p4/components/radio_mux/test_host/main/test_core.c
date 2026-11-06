#include <stdlib.h>
#include <string.h>
#include "radio_mux_core.h"
#include "unity.h"
#include "unity_test_runner.h"

#define MODE_ZIGGURAT RADIO_MUX_MODE_ZIGGURAT
#define N_CHANS 3

static const uint32_t ACCEPTED_BAUDS[] = {38400, 57600, 115200, 230400, 460800, 921600, 1000000, 2000000};

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// No registered consumer: the build as it is today.
static radio_mux_rules_t no_rules(void)
{
    radio_mux_rules_t r = {.n = 0};

    return r;
}

// A ziggurat consumer: needs spinel, one channel, no reboot.
static radio_mux_rules_t ziggurat_rules(void)
{
    radio_mux_rules_t r = {.n = 1};

    r.rules[0] = (radio_mux_rule_t) {
        .mode = RADIO_MUX_MODE_ZIGGURAT, .needs = RADIO_MUX_FW_SPINEL, .limit = 1, .reboot_on_change = false,
    };
    return r;
}

static radio_mux_chan_view_t chan(const char *id, radio_mux_mode_t mode, radio_mux_fw_t fw)
{
    return (radio_mux_chan_view_t) {
        .id = id, .mode = mode, .present = true, .supported = true, .fw = fw
    };
}

// Channels: [0] c6 (otbr, spinel), [1] a spinel dongle (off), [2] a ZNP dongle (off).
static void three_channels(radio_mux_chan_view_t c[N_CHANS])
{
    c[0] = chan("c6", RADIO_MUX_MODE_OTBR, RADIO_MUX_FW_SPINEL);
    c[1] = chan("usb-303a-1001", RADIO_MUX_MODE_OFF, RADIO_MUX_FW_SPINEL);
    c[2] = chan("usb-10c4-ea60", RADIO_MUX_MODE_OFF, RADIO_MUX_FW_ZNP);
}

/* ---- radio_mux_channel_id ---- */

TEST_CASE("On a USB device with a serial string: returns usb-<vid>-<pid>-<serial> in lowercase hex, serial reduced "
          "to [A-Za-z0-9]", "[radio_mux]")
{
    char id[RADIO_MUX_ID_MAX];

    radio_mux_channel_id(0x303A, 0x1001, "AB:12-cd_3 4", id);
    TEST_ASSERT_EQUAL_STRING("usb-303a-1001-AB12cd34", id);
    radio_mux_channel_id(0x0A0B, 0x00FF, "X", id);
    TEST_ASSERT_EQUAL_STRING("usb-0a0b-00ff-X", id);
}

TEST_CASE("On a USB device without a serial string: returns usb-<vid>-<pid>", "[radio_mux]")
{
    char id[RADIO_MUX_ID_MAX];

    radio_mux_channel_id(0x10C4, 0xEA60, NULL, id);
    TEST_ASSERT_EQUAL_STRING("usb-10c4-ea60", id);
    radio_mux_channel_id(0x10C4, 0xEA60, "", id);
    TEST_ASSERT_EQUAL_STRING("usb-10c4-ea60", id);
    radio_mux_channel_id(0x10C4, 0xEA60, ":-_ ", id);
    TEST_ASSERT_EQUAL_STRING("usb-10c4-ea60", id);
}

TEST_CASE("On a serial that would exceed 47 characters: truncates the serial so the id is 47 characters",
          "[radio_mux]")
{
    char id[RADIO_MUX_ID_MAX];
    const char *serial = "0123456789abcdefghijklmnopqrstuvwxyz0123456789";

    memset(id, 0x7F, sizeof(id));
    radio_mux_channel_id(0x303A, 0x1001, serial, id);
    TEST_ASSERT_EQUAL(47, strlen(id));
    TEST_ASSERT_EQUAL_STRING("usb-303a-1001-0123456789abcdefghijklmnopqrstuvw", id);
}

/* ---- radio_mux_available_modes ---- */

TEST_CASE("On no rules: only off is available", "[radio_mux]")
{
    const radio_mux_rules_t rules = {0};

    TEST_ASSERT_EQUAL_UINT32(RADIO_MUX_MODE_BIT(RADIO_MUX_MODE_OFF), radio_mux_available_modes(&rules));
}

TEST_CASE("On rules for otbr and rcp-proxy: off, otbr and rcp-proxy are available, ziggurat is not", "[radio_mux]")
{
    radio_mux_rules_t rules = {0};

    rules.rules[rules.n++] = (radio_mux_rule_t) {
        .mode = RADIO_MUX_MODE_OTBR
    };
    rules.rules[rules.n++] = (radio_mux_rule_t) {
        .mode = RADIO_MUX_MODE_RCP_PROXY
    };
    TEST_ASSERT_EQUAL_UINT32(RADIO_MUX_MODE_BIT(RADIO_MUX_MODE_OFF) | RADIO_MUX_MODE_BIT(RADIO_MUX_MODE_OTBR) |
                             RADIO_MUX_MODE_BIT(RADIO_MUX_MODE_RCP_PROXY),
                             radio_mux_available_modes(&rules));
}

TEST_CASE("On a ziggurat rule too: ziggurat is available", "[radio_mux]")
{
    radio_mux_rules_t rules = {0};

    rules.rules[rules.n++] = (radio_mux_rule_t) {
        .mode = RADIO_MUX_MODE_ZIGGURAT
    };
    TEST_ASSERT_TRUE(radio_mux_available_modes(&rules) & RADIO_MUX_MODE_BIT(RADIO_MUX_MODE_ZIGGURAT));
}

TEST_CASE("On a rule for a non-mode (unavailable, invalid): it is ignored", "[radio_mux]")
{
    radio_mux_rules_t rules = {0};

    rules.rules[rules.n++] = (radio_mux_rule_t) {
        .mode = RADIO_MUX_MODE_UNAVAILABLE
    };
    rules.rules[rules.n++] = (radio_mux_rule_t) {
        .mode = RADIO_MUX_MODE_INVALID
    };
    TEST_ASSERT_EQUAL_UINT32(RADIO_MUX_MODE_BIT(RADIO_MUX_MODE_OFF), radio_mux_available_modes(&rules));
}

/* ---- radio_mux_mode_parse / radio_mux_mode_name ---- */

TEST_CASE("On each wire name: round-trips unavailable, off, otbr, ziggurat, rcp-proxy", "[radio_mux]")
{
    const char *names[] = {"unavailable", "off", "otbr", "ziggurat", "rcp-proxy"};
    const radio_mux_mode_t modes[] = {RADIO_MUX_MODE_UNAVAILABLE, RADIO_MUX_MODE_OFF, RADIO_MUX_MODE_OTBR,
                                      RADIO_MUX_MODE_ZIGGURAT, RADIO_MUX_MODE_RCP_PROXY
                                     };

    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        TEST_ASSERT_EQUAL(modes[i], radio_mux_mode_parse(names[i]));
        TEST_ASSERT_EQUAL_STRING(names[i], radio_mux_mode_name(modes[i]));
    }
}

TEST_CASE("On an unknown name: returns RADIO_MUX_MODE_INVALID", "[radio_mux]")
{
    TEST_ASSERT_EQUAL(RADIO_MUX_MODE_INVALID, radio_mux_mode_parse("OTBR"));
    TEST_ASSERT_EQUAL(RADIO_MUX_MODE_INVALID, radio_mux_mode_parse("rcp_proxy"));
    TEST_ASSERT_EQUAL(RADIO_MUX_MODE_INVALID, radio_mux_mode_parse(""));
    TEST_ASSERT_EQUAL(RADIO_MUX_MODE_INVALID, radio_mux_mode_parse(NULL));
    TEST_ASSERT_NULL(radio_mux_mode_name(RADIO_MUX_MODE_INVALID));
}

TEST_CASE("On each firmware type: names it unknown, spinel, ziggurat, znp, ezsp", "[radio_mux]")
{
    TEST_ASSERT_EQUAL_STRING("unknown", radio_mux_fw_name(RADIO_MUX_FW_UNKNOWN));
    TEST_ASSERT_EQUAL_STRING("spinel", radio_mux_fw_name(RADIO_MUX_FW_SPINEL));
    TEST_ASSERT_EQUAL_STRING("ziggurat", radio_mux_fw_name(RADIO_MUX_FW_ZIGGURAT));
    TEST_ASSERT_EQUAL_STRING("znp", radio_mux_fw_name(RADIO_MUX_FW_ZNP));
    TEST_ASSERT_EQUAL_STRING("ezsp", radio_mux_fw_name(RADIO_MUX_FW_EZSP));
}

/* ---- radio_mux_fits ---- */

TEST_CASE("On otbr with spinel: returns true", "[radio_mux]")
{
    radio_mux_rules_t none = no_rules();

    TEST_ASSERT_TRUE(radio_mux_fits(&none, RADIO_MUX_MODE_OTBR, RADIO_MUX_FW_SPINEL));
}

TEST_CASE("On otbr with znp, ezsp, ziggurat or unknown: returns false", "[radio_mux]")
{
    radio_mux_rules_t none = no_rules();

    TEST_ASSERT_FALSE(radio_mux_fits(&none, RADIO_MUX_MODE_OTBR, RADIO_MUX_FW_ZNP));
    TEST_ASSERT_FALSE(radio_mux_fits(&none, RADIO_MUX_MODE_OTBR, RADIO_MUX_FW_EZSP));
    TEST_ASSERT_FALSE(radio_mux_fits(&none, RADIO_MUX_MODE_OTBR, RADIO_MUX_FW_ZIGGURAT));
    TEST_ASSERT_FALSE(radio_mux_fits(&none, RADIO_MUX_MODE_OTBR, RADIO_MUX_FW_UNKNOWN));
}

TEST_CASE("On rcp-proxy or off with any type, unknown included: returns true", "[radio_mux]")
{
    radio_mux_rules_t none = no_rules();
    const radio_mux_fw_t fws[] = {RADIO_MUX_FW_UNKNOWN, RADIO_MUX_FW_SPINEL, RADIO_MUX_FW_ZIGGURAT, RADIO_MUX_FW_ZNP,
                                  RADIO_MUX_FW_EZSP
                                 };

    for (size_t i = 0; i < sizeof(fws) / sizeof(fws[0]); i++) {
        TEST_ASSERT_TRUE(radio_mux_fits(&none, RADIO_MUX_MODE_RCP_PROXY, fws[i]));
        TEST_ASSERT_TRUE(radio_mux_fits(&none, RADIO_MUX_MODE_OFF, fws[i]));
    }
}

TEST_CASE("On a registered mode: uses the type its registration declares", "[radio_mux]")
{
    radio_mux_rules_t rules = ziggurat_rules();
    radio_mux_rules_t any = ziggurat_rules();
    radio_mux_rules_t none = no_rules();

    any.rules[0].needs = RADIO_MUX_FW_UNKNOWN;
    TEST_ASSERT_TRUE(radio_mux_fits(&rules, MODE_ZIGGURAT, RADIO_MUX_FW_SPINEL));
    TEST_ASSERT_FALSE(radio_mux_fits(&rules, MODE_ZIGGURAT, RADIO_MUX_FW_ZNP));
    TEST_ASSERT_TRUE(radio_mux_fits(&any, MODE_ZIGGURAT, RADIO_MUX_FW_ZNP));
    // Without a registration the mode fits nothing.
    TEST_ASSERT_FALSE(radio_mux_fits(&none, MODE_ZIGGURAT, RADIO_MUX_FW_SPINEL));
}

/* ---- radio_mux_check_set_mode ---- */

TEST_CASE("On unavailable as the requested mode: returns RADIO_MUX_REFUSE_BAD_VALUE", "[radio_mux]")
{
    radio_mux_rules_t none = no_rules();
    radio_mux_chan_view_t c[N_CHANS];

    three_channels(c);
    TEST_ASSERT_EQUAL(RADIO_MUX_REFUSE_BAD_VALUE,
                      radio_mux_check_set_mode(&none, c, N_CHANS, 1, RADIO_MUX_MODE_UNAVAILABLE, NULL));
    TEST_ASSERT_EQUAL(RADIO_MUX_REFUSE_BAD_VALUE,
                      radio_mux_check_set_mode(&none, c, N_CHANS, 1, RADIO_MUX_MODE_INVALID, NULL));
}

TEST_CASE("On any mode for an unavailable channel: returns RADIO_MUX_REFUSE_UNAVAILABLE", "[radio_mux]")
{
    radio_mux_rules_t none = no_rules();
    radio_mux_chan_view_t c[N_CHANS];

    three_channels(c);
    c[1].present = false;
    TEST_ASSERT_EQUAL(RADIO_MUX_REFUSE_UNAVAILABLE,
                      radio_mux_check_set_mode(&none, c, N_CHANS, 1, RADIO_MUX_MODE_RCP_PROXY, NULL));
    TEST_ASSERT_EQUAL(RADIO_MUX_REFUSE_UNAVAILABLE,
                      radio_mux_check_set_mode(&none, c, N_CHANS, 1, RADIO_MUX_MODE_OFF, NULL));
    c[1].present = true;
    c[1].supported = false;
    TEST_ASSERT_EQUAL(RADIO_MUX_REFUSE_UNAVAILABLE,
                      radio_mux_check_set_mode(&none, c, N_CHANS, 1, RADIO_MUX_MODE_OTBR, NULL));
}

TEST_CASE("On a mode with no registered consumer (ziggurat in this build): returns RADIO_MUX_REFUSE_NOT_AVAILABLE",
          "[radio_mux]")
{
    radio_mux_rules_t none = no_rules();
    radio_mux_chan_view_t c[N_CHANS];

    three_channels(c);
    TEST_ASSERT_EQUAL(RADIO_MUX_REFUSE_NOT_AVAILABLE,
                      radio_mux_check_set_mode(&none, c, N_CHANS, 1, MODE_ZIGGURAT, NULL));
}

TEST_CASE("On a mode the detected firmware doesn't fit: returns RADIO_MUX_REFUSE_INCOMPATIBLE", "[radio_mux]")
{
    radio_mux_rules_t zig = ziggurat_rules();
    radio_mux_chan_view_t c[N_CHANS];

    three_channels(c);
    TEST_ASSERT_EQUAL(RADIO_MUX_REFUSE_INCOMPATIBLE,
                      radio_mux_check_set_mode(&zig, c, N_CHANS, 2, RADIO_MUX_MODE_OTBR, NULL));
    TEST_ASSERT_EQUAL(RADIO_MUX_REFUSE_INCOMPATIBLE,
                      radio_mux_check_set_mode(&zig, c, N_CHANS, 2, MODE_ZIGGURAT, NULL));
    c[2].fw = RADIO_MUX_FW_UNKNOWN;
    TEST_ASSERT_EQUAL(RADIO_MUX_REFUSE_INCOMPATIBLE,
                      radio_mux_check_set_mode(&zig, c, N_CHANS, 2, RADIO_MUX_MODE_OTBR, NULL));
}

TEST_CASE("On otbr while another channel is otbr: returns RADIO_MUX_MOVE with the other channel as the source",
          "[radio_mux]")
{
    radio_mux_rules_t none = no_rules();
    radio_mux_chan_view_t c[N_CHANS];
    size_t from = 99;

    three_channels(c);
    TEST_ASSERT_EQUAL(RADIO_MUX_MOVE, radio_mux_check_set_mode(&none, c, N_CHANS, 1, RADIO_MUX_MODE_OTBR, &from));
    TEST_ASSERT_EQUAL(0, from);
    // The same source when the caller does not ask for it.
    TEST_ASSERT_EQUAL(RADIO_MUX_MOVE, radio_mux_check_set_mode(&none, c, N_CHANS, 1, RADIO_MUX_MODE_OTBR, NULL));
    // Moving back: the source is the dongle.
    c[0].mode = RADIO_MUX_MODE_OFF;
    c[1].mode = RADIO_MUX_MODE_OTBR;
    TEST_ASSERT_EQUAL(RADIO_MUX_MOVE, radio_mux_check_set_mode(&none, c, N_CHANS, 0, RADIO_MUX_MODE_OTBR, &from));
    TEST_ASSERT_EQUAL(1, from);
}

TEST_CASE("On otbr for the channel that already is otbr: returns RADIO_MUX_APPLY", "[radio_mux]")
{
    radio_mux_rules_t none = no_rules();
    radio_mux_chan_view_t c[N_CHANS];
    size_t from = 99;

    three_channels(c);
    TEST_ASSERT_EQUAL(RADIO_MUX_APPLY, radio_mux_check_set_mode(&none, c, N_CHANS, 0, RADIO_MUX_MODE_OTBR, &from));
    TEST_ASSERT_EQUAL(99, from);
}

TEST_CASE("On a registered limit-1 mode already used by another channel: returns RADIO_MUX_REFUSE_MODE_TAKEN",
          "[radio_mux]")
{
    radio_mux_rules_t zig = ziggurat_rules();
    radio_mux_chan_view_t c[N_CHANS];

    three_channels(c);
    c[0].mode = RADIO_MUX_MODE_OFF;
    c[1].mode = MODE_ZIGGURAT;
    c[2].fw = RADIO_MUX_FW_SPINEL;
    TEST_ASSERT_EQUAL(RADIO_MUX_REFUSE_MODE_TAKEN,
                      radio_mux_check_set_mode(&zig, c, N_CHANS, 2, MODE_ZIGGURAT, NULL));
    // The channel that has the mode may set it again, and a higher limit lets a second channel in.
    TEST_ASSERT_EQUAL(RADIO_MUX_APPLY, radio_mux_check_set_mode(&zig, c, N_CHANS, 1, MODE_ZIGGURAT, NULL));
    zig.rules[0].limit = 2;
    TEST_ASSERT_EQUAL(RADIO_MUX_APPLY, radio_mux_check_set_mode(&zig, c, N_CHANS, 2, MODE_ZIGGURAT, NULL));
    zig.rules[0].limit = 0;
    TEST_ASSERT_EQUAL(RADIO_MUX_APPLY, radio_mux_check_set_mode(&zig, c, N_CHANS, 2, MODE_ZIGGURAT, NULL));
}

TEST_CASE("On otbr while a ziggurat consumer is active, or ziggurat while otbr is active: returns "
          "RADIO_MUX_REFUSE_HYBRID", "[radio_mux]")
{
    radio_mux_rules_t zig = ziggurat_rules();
    radio_mux_chan_view_t c[N_CHANS];

    // ziggurat requested on the dongle while the c6 is otbr
    three_channels(c);
    TEST_ASSERT_EQUAL(RADIO_MUX_REFUSE_HYBRID, radio_mux_check_set_mode(&zig, c, N_CHANS, 1, MODE_ZIGGURAT, NULL));
    // otbr requested on the c6 while the dongle runs ziggurat
    c[0].mode = RADIO_MUX_MODE_OFF;
    c[1].mode = MODE_ZIGGURAT;
    TEST_ASSERT_EQUAL(RADIO_MUX_REFUSE_HYBRID, radio_mux_check_set_mode(&zig, c, N_CHANS, 0, RADIO_MUX_MODE_OTBR, NULL));
    // A channel leaving ziggurat for otbr is not a hybrid: nothing else runs ziggurat.
    TEST_ASSERT_EQUAL(RADIO_MUX_APPLY, radio_mux_check_set_mode(&zig, c, N_CHANS, 1, RADIO_MUX_MODE_OTBR, NULL));
}

TEST_CASE("On a mode whose registration declares reboot on enter or leave: returns RADIO_MUX_APPLY_REBOOT",
          "[radio_mux]")
{
    radio_mux_rules_t zig = ziggurat_rules();
    radio_mux_chan_view_t c[N_CHANS];

    zig.rules[0].reboot_on_change = true;
    three_channels(c);
    c[0].mode = RADIO_MUX_MODE_OFF;
    // enter
    TEST_ASSERT_EQUAL(RADIO_MUX_APPLY_REBOOT, radio_mux_check_set_mode(&zig, c, N_CHANS, 1, MODE_ZIGGURAT, NULL));
    // leave
    c[1].mode = MODE_ZIGGURAT;
    TEST_ASSERT_EQUAL(RADIO_MUX_APPLY_REBOOT, radio_mux_check_set_mode(&zig, c, N_CHANS, 1, RADIO_MUX_MODE_OFF, NULL));
    TEST_ASSERT_EQUAL(RADIO_MUX_APPLY_REBOOT,
                      radio_mux_check_set_mode(&zig, c, N_CHANS, 1, RADIO_MUX_MODE_RCP_PROXY, NULL));
}

TEST_CASE("On off or rcp-proxy otherwise: returns RADIO_MUX_APPLY", "[radio_mux]")
{
    radio_mux_rules_t none = no_rules();
    radio_mux_rules_t zig = ziggurat_rules();
    radio_mux_chan_view_t c[N_CHANS];

    three_channels(c);
    // Any firmware, including one otbr does not fit, and the otbr channel itself.
    TEST_ASSERT_EQUAL(RADIO_MUX_APPLY, radio_mux_check_set_mode(&none, c, N_CHANS, 2, RADIO_MUX_MODE_RCP_PROXY, NULL));
    TEST_ASSERT_EQUAL(RADIO_MUX_APPLY, radio_mux_check_set_mode(&none, c, N_CHANS, 2, RADIO_MUX_MODE_OFF, NULL));
    TEST_ASSERT_EQUAL(RADIO_MUX_APPLY, radio_mux_check_set_mode(&none, c, N_CHANS, 0, RADIO_MUX_MODE_OFF, NULL));
    // Leaving a registered mode without a reboot declaration.
    c[0].mode = RADIO_MUX_MODE_OFF;
    c[1].mode = MODE_ZIGGURAT;
    TEST_ASSERT_EQUAL(RADIO_MUX_APPLY, radio_mux_check_set_mode(&zig, c, N_CHANS, 1, RADIO_MUX_MODE_OFF, NULL));
    // otbr onto a free spinel channel, no other otbr.
    c[1].mode = RADIO_MUX_MODE_OFF;
    c[0].mode = RADIO_MUX_MODE_OFF;
    TEST_ASSERT_EQUAL(RADIO_MUX_APPLY, radio_mux_check_set_mode(&none, c, N_CHANS, 1, RADIO_MUX_MODE_OTBR, NULL));
}

/* ---- radio_mux_after_detect ---- */

TEST_CASE("On a stored mode the detected type fits: returns the stored mode", "[radio_mux]")
{
    radio_mux_rules_t zig = ziggurat_rules();
    char reason[96] = "stale";

    TEST_ASSERT_EQUAL(RADIO_MUX_MODE_OTBR,
                      radio_mux_after_detect(&zig, RADIO_MUX_MODE_OTBR, RADIO_MUX_FW_SPINEL, reason, sizeof(reason)));
    TEST_ASSERT_EQUAL_STRING("", reason);
    TEST_ASSERT_EQUAL(MODE_ZIGGURAT,
                      radio_mux_after_detect(&zig, MODE_ZIGGURAT, RADIO_MUX_FW_SPINEL, reason, sizeof(reason)));
    TEST_ASSERT_EQUAL(RADIO_MUX_MODE_RCP_PROXY,
                      radio_mux_after_detect(&zig, RADIO_MUX_MODE_RCP_PROXY, RADIO_MUX_FW_UNKNOWN, reason,
                                             sizeof(reason)));
    TEST_ASSERT_EQUAL(RADIO_MUX_MODE_OFF,
                      radio_mux_after_detect(&zig, RADIO_MUX_MODE_OFF, RADIO_MUX_FW_ZNP, reason, sizeof(reason)));
}

TEST_CASE("On a stored mode it doesn't fit: returns off, with the reason \"stored mode <mode> needs <type>; "
          "detected <type>\"", "[radio_mux]")
{
    radio_mux_rules_t zig = ziggurat_rules();
    char reason[96];

    TEST_ASSERT_EQUAL(RADIO_MUX_MODE_OFF,
                      radio_mux_after_detect(&zig, RADIO_MUX_MODE_OTBR, RADIO_MUX_FW_ZNP, reason, sizeof(reason)));
    TEST_ASSERT_EQUAL_STRING("stored mode otbr needs spinel; detected znp", reason);
    TEST_ASSERT_EQUAL(RADIO_MUX_MODE_OFF,
                      radio_mux_after_detect(&zig, MODE_ZIGGURAT, RADIO_MUX_FW_EZSP, reason, sizeof(reason)));
    TEST_ASSERT_EQUAL_STRING("stored mode ziggurat needs spinel; detected ezsp", reason);
    TEST_ASSERT_EQUAL(RADIO_MUX_MODE_OFF,
                      radio_mux_after_detect(&zig, RADIO_MUX_MODE_OTBR, RADIO_MUX_FW_UNKNOWN, reason, sizeof(reason)));
    TEST_ASSERT_EQUAL_STRING("stored mode otbr needs spinel; detected unknown", reason);
}

TEST_CASE("On a stored mode of an unregistered consumer: returns off, with the reason \"<mode> is not in this "
          "build\"", "[radio_mux]")
{
    radio_mux_rules_t none = no_rules();
    char reason[96];

    TEST_ASSERT_EQUAL(RADIO_MUX_MODE_OFF,
                      radio_mux_after_detect(&none, MODE_ZIGGURAT, RADIO_MUX_FW_SPINEL, reason, sizeof(reason)));
    TEST_ASSERT_EQUAL_STRING("ziggurat is not in this build", reason);
}

TEST_CASE("On a reason longer than the buffer: truncates it with a NUL and a NULL buffer is accepted", "[radio_mux]")
{
    radio_mux_rules_t none = no_rules();
    char reason[10];

    TEST_ASSERT_EQUAL(RADIO_MUX_MODE_OFF,
                      radio_mux_after_detect(&none, RADIO_MUX_MODE_OTBR, RADIO_MUX_FW_ZNP, reason, sizeof(reason)));
    TEST_ASSERT_EQUAL(9, strlen(reason));
    TEST_ASSERT_EQUAL(RADIO_MUX_MODE_OFF, radio_mux_after_detect(&none, RADIO_MUX_MODE_OTBR, RADIO_MUX_FW_ZNP, NULL, 0));
}

/* ---- radio_mux_proxy_defaults ---- */

TEST_CASE("On a call: gives direction listen, adapter tcp, port 6638, empty host, framing hdlc", "[radio_mux]")
{
    radio_mux_proxy_settings_t s;

    memset(&s, 0xEE, sizeof(s));
    radio_mux_proxy_defaults(&s);
    TEST_ASSERT_EQUAL(RADIO_MUX_PROXY_LISTEN, s.dir);
    TEST_ASSERT_EQUAL(RADIO_MUX_PROXY_TCP, s.adapter);
    TEST_ASSERT_EQUAL_UINT16(6638, s.port);
    TEST_ASSERT_EQUAL_STRING("", s.host);
    TEST_ASSERT_EQUAL(RADIO_MUX_PROXY_HDLC, s.framing);
}

/* ---- radio_mux_baud_ok ---- */

TEST_CASE("On each accepted baud rate: returns true", "[radio_mux]")
{
    for (size_t i = 0; i < sizeof(ACCEPTED_BAUDS) / sizeof(ACCEPTED_BAUDS[0]); i++) {
        TEST_ASSERT_TRUE_MESSAGE(radio_mux_baud_ok(ACCEPTED_BAUDS[i]), "accepted baud rate refused");
    }
}

TEST_CASE("On a baud rate not in the list (0, 9600, 115201, 3000000): returns false", "[radio_mux]")
{
    TEST_ASSERT_FALSE(radio_mux_baud_ok(0));
    TEST_ASSERT_FALSE(radio_mux_baud_ok(9600));
    TEST_ASSERT_FALSE(radio_mux_baud_ok(115201));
    TEST_ASSERT_FALSE(radio_mux_baud_ok(3000000));
}

/* ---- radio_mux_pick_driver ---- */

TEST_CASE("On 10c4:ea60, 10c4:ea70, 10c4:ea71: returns RADIO_MUX_DRV_CP210X", "[radio_mux]")
{
    const uint8_t intf[] = {0xFF};

    TEST_ASSERT_EQUAL(RADIO_MUX_DRV_CP210X, radio_mux_pick_driver(0x10C4, 0xEA60, 0x00, intf, 1));
    TEST_ASSERT_EQUAL(RADIO_MUX_DRV_CP210X, radio_mux_pick_driver(0x10C4, 0xEA70, 0x00, intf, 1));
    TEST_ASSERT_EQUAL(RADIO_MUX_DRV_CP210X, radio_mux_pick_driver(0x10C4, 0xEA71, 0x00, intf, 1));
}

TEST_CASE("On 1a86:7522, 1a86:7523, 1a86:5523: returns RADIO_MUX_DRV_CH34X", "[radio_mux]")
{
    const uint8_t intf[] = {0xFF};

    TEST_ASSERT_EQUAL(RADIO_MUX_DRV_CH34X, radio_mux_pick_driver(0x1A86, 0x7522, 0xFF, intf, 1));
    TEST_ASSERT_EQUAL(RADIO_MUX_DRV_CH34X, radio_mux_pick_driver(0x1A86, 0x7523, 0xFF, intf, 1));
    TEST_ASSERT_EQUAL(RADIO_MUX_DRV_CH34X, radio_mux_pick_driver(0x1A86, 0x5523, 0xFF, intf, 1));
}

TEST_CASE("On a device with interfaces of class 0x02 and 0x0A (any VID): returns RADIO_MUX_DRV_CDC_ACM",
          "[radio_mux]")
{
    const uint8_t intf[] = {0x02, 0x0A};

    TEST_ASSERT_EQUAL(RADIO_MUX_DRV_CDC_ACM, radio_mux_pick_driver(0x303A, 0x1001, 0x00, intf, 2));
    TEST_ASSERT_EQUAL(RADIO_MUX_DRV_CDC_ACM, radio_mux_pick_driver(0x1234, 0x5678, 0xEF, intf, 2));
}

TEST_CASE("On a device of class 0x02 with one interface of class 0x0A: returns RADIO_MUX_DRV_CDC_ACM",
          "[radio_mux]")
{
    const uint8_t intf[] = {0x0A};

    TEST_ASSERT_EQUAL(RADIO_MUX_DRV_CDC_ACM, radio_mux_pick_driver(0x2341, 0x0043, 0x02, intf, 1));
}

TEST_CASE("On an FTDI device (0403:6001, vendor class 0xFF): returns RADIO_MUX_DRV_NONE", "[radio_mux]")
{
    const uint8_t intf[] = {0xFF};

    TEST_ASSERT_EQUAL(RADIO_MUX_DRV_NONE, radio_mux_pick_driver(0x0403, 0x6001, 0xFF, intf, 1));
}

TEST_CASE("On a mass-storage device (interface class 0x08): returns RADIO_MUX_DRV_NONE", "[radio_mux]")
{
    const uint8_t intf[] = {0x08};

    TEST_ASSERT_EQUAL(RADIO_MUX_DRV_NONE, radio_mux_pick_driver(0x0781, 0x5567, 0x00, intf, 1));
}

TEST_CASE("On a device with only a CDC data interface and no communications class: returns RADIO_MUX_DRV_NONE",
          "[radio_mux]")
{
    const uint8_t intf[] = {0x0A};

    TEST_ASSERT_EQUAL(RADIO_MUX_DRV_NONE, radio_mux_pick_driver(0x1234, 0x5678, 0x00, intf, 1));
    TEST_ASSERT_EQUAL(RADIO_MUX_DRV_NONE, radio_mux_pick_driver(0x1234, 0x5678, 0x00, NULL, 0));
}

/* ---- radio_mux_driver_name ---- */

TEST_CASE("On each driver: returns \"cp210x\", \"ch34x\", \"cdc_acm\", or NULL for NONE", "[radio_mux]")
{
    TEST_ASSERT_EQUAL_STRING("cp210x", radio_mux_driver_name(RADIO_MUX_DRV_CP210X));
    TEST_ASSERT_EQUAL_STRING("ch34x", radio_mux_driver_name(RADIO_MUX_DRV_CH34X));
    TEST_ASSERT_EQUAL_STRING("cdc_acm", radio_mux_driver_name(RADIO_MUX_DRV_CDC_ACM));
    TEST_ASSERT_NULL(radio_mux_driver_name(RADIO_MUX_DRV_NONE));
}

/* ---- radio_mux_baud_verdict ---- */

TEST_CASE("On actual == requested: returns RADIO_MUX_BAUD_EXACT", "[radio_mux]")
{
    TEST_ASSERT_EQUAL(RADIO_MUX_BAUD_EXACT, radio_mux_baud_verdict(115200, 115200));
}

TEST_CASE("On 921600 read back as 923076: returns RADIO_MUX_BAUD_CLOSE", "[radio_mux]")
{
    TEST_ASSERT_EQUAL(RADIO_MUX_BAUD_CLOSE, radio_mux_baud_verdict(921600, 923076));
}

TEST_CASE("On 115200 read back as 9600, and on actual 0: returns RADIO_MUX_BAUD_FAR", "[radio_mux]")
{
    TEST_ASSERT_EQUAL(RADIO_MUX_BAUD_FAR, radio_mux_baud_verdict(115200, 9600));
    TEST_ASSERT_EQUAL(RADIO_MUX_BAUD_FAR, radio_mux_baud_verdict(115200, 0));
}

/* ---- radio_mux_cp210x_flow ---- */

TEST_CASE("On flow control off: writes ulControlHandshake 0x00000001, ulFlowReplace 0x00000040, limits 128",
          "[radio_mux]")
{
    const uint8_t expected[RADIO_MUX_CP210X_FLOW_LEN] = {
        0x01, 0x00, 0x00, 0x00,
        0x40, 0x00, 0x00, 0x00,
        0x80, 0x00, 0x00, 0x00,
        0x80, 0x00, 0x00, 0x00,
    };
    uint8_t out[RADIO_MUX_CP210X_FLOW_LEN];

    memset(out, 0xEE, sizeof(out));
    radio_mux_cp210x_flow(false, out);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, out, sizeof(expected));
    TEST_ASSERT_EQUAL_UINT32(0x00000001, le32(&out[0]));
}

TEST_CASE("On flow control on: writes ulControlHandshake 0x00000009, ulFlowReplace 0x00000080, limits 128",
          "[radio_mux]")
{
    const uint8_t expected[RADIO_MUX_CP210X_FLOW_LEN] = {
        0x09, 0x00, 0x00, 0x00,
        0x80, 0x00, 0x00, 0x00,
        0x80, 0x00, 0x00, 0x00,
        0x80, 0x00, 0x00, 0x00,
    };
    uint8_t out[RADIO_MUX_CP210X_FLOW_LEN];

    memset(out, 0xEE, sizeof(out));
    radio_mux_cp210x_flow(true, out);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, out, sizeof(expected));
    TEST_ASSERT_EQUAL_UINT32(0x00000009, le32(&out[0]));
}

/* ---- attach handles ---- */

TEST_CASE("On the next generation: returns gen + 1", "[radio_mux]")
{
    TEST_ASSERT_EQUAL_UINT32(1, radio_mux_gen_next(0));
    TEST_ASSERT_EQUAL_UINT32(2, radio_mux_gen_next(1));
    TEST_ASSERT_EQUAL_UINT32(RADIO_MUX_GEN_MAX, radio_mux_gen_next(RADIO_MUX_GEN_MAX - 1));
}

TEST_CASE("On the next generation after RADIO_MUX_GEN_MAX: wraps to 1, never 0", "[radio_mux]")
{
    TEST_ASSERT_EQUAL_UINT32(1, radio_mux_gen_next(RADIO_MUX_GEN_MAX));
    TEST_ASSERT_EQUAL_UINT32(1, radio_mux_gen_next(UINT32_MAX));
}

TEST_CASE("On a handle packed and unpacked: returns its slot and generation", "[radio_mux]")
{
    const size_t slots[] = {0, 1, RADIO_MUX_HANDLE_SLOTS - 1};
    const uint32_t gens[] = {1, 2, 12345, RADIO_MUX_GEN_MAX};

    for (size_t i = 0; i < sizeof(slots) / sizeof(slots[0]); i++) {
        for (size_t j = 0; j < sizeof(gens) / sizeof(gens[0]); j++) {
            const uintptr_t h = radio_mux_handle_pack(slots[i], gens[j]);
            size_t slot = 99;
            uint32_t gen = 0;

            TEST_ASSERT_NOT_EQUAL(0, h);
            TEST_ASSERT_TRUE(h <= UINT32_MAX); // fits the board's 32-bit pointers
            TEST_ASSERT_TRUE(radio_mux_handle_unpack(h, &slot, &gen));
            TEST_ASSERT_EQUAL(slots[i], slot);
            TEST_ASSERT_EQUAL_UINT32(gens[j], gen);
        }
    }
}

TEST_CASE("On two attaches of the same slot: returns different handles", "[radio_mux]")
{
    TEST_ASSERT_NOT_EQUAL(radio_mux_handle_pack(1, 7), radio_mux_handle_pack(1, radio_mux_gen_next(7)));
    TEST_ASSERT_NOT_EQUAL(radio_mux_handle_pack(0, 7), radio_mux_handle_pack(1, 7));
}

TEST_CASE("On a slot or generation out of range: returns the handle value 0", "[radio_mux]")
{
    TEST_ASSERT_EQUAL(0, radio_mux_handle_pack(RADIO_MUX_HANDLE_SLOTS, 1));
    TEST_ASSERT_EQUAL(0, radio_mux_handle_pack(0, 0));
    TEST_ASSERT_EQUAL(0, radio_mux_handle_pack(0, RADIO_MUX_GEN_MAX + 1));
}

TEST_CASE("On unpacking 0 or a generation 0: returns false and sets nothing", "[radio_mux]")
{
    size_t slot = 99;
    uint32_t gen = 99;

    TEST_ASSERT_FALSE(radio_mux_handle_unpack(0, &slot, &gen));
    TEST_ASSERT_FALSE(radio_mux_handle_unpack(RADIO_MUX_HANDLE_SLOTS - 1, &slot, &gen));
    TEST_ASSERT_EQUAL(99, slot);
    TEST_ASSERT_EQUAL_UINT32(99, gen);
}

TEST_CASE("On a handle of the transport's current attach: returns true", "[radio_mux]")
{
    TEST_ASSERT_TRUE(radio_mux_handle_current(5, 5));
}

TEST_CASE("On a handle of an earlier attach: returns false", "[radio_mux]")
{
    TEST_ASSERT_FALSE(radio_mux_handle_current(5, radio_mux_gen_next(5)));
    TEST_ASSERT_FALSE(radio_mux_handle_current(radio_mux_gen_next(5), 5));
}

TEST_CASE("On a transport nothing ever attached to: returns false", "[radio_mux]")
{
    TEST_ASSERT_FALSE(radio_mux_handle_current(0, 0));
    TEST_ASSERT_FALSE(radio_mux_handle_current(1, 0));
}

void app_main(void)
{
    UNITY_BEGIN();
    unity_run_all_tests();
    int failures = (int)UNITY_END();
    exit(failures == 0 ? 0 : 1);
}
