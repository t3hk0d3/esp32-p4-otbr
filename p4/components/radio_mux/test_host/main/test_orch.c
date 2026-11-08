// The orchestrator's pure core: binding rules, which instance binds, the load dedupe, the reinit run and its texts.
#include <string.h>
#include "radio_mux_orch_core.h"
#include "unity.h"
#include "unity_test_runner.h"

#define MODE_ZIGGURAT RADIO_MUX_MODE_ZIGGURAT
#define N_CHANS 3
#define NOW 1000000

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
    c[0] = chan("builtin-c6", RADIO_MUX_MODE_OTBR, RADIO_MUX_FW_SPINEL);
    c[1] = chan("usb-303a-1001", RADIO_MUX_MODE_OFF, RADIO_MUX_FW_SPINEL);
    c[2] = chan("usb-10c4-ea60", RADIO_MUX_MODE_OFF, RADIO_MUX_FW_ZNP);
}

/* ---- radio_mux_orch_check_bind (today's radio_mux_check_set_mode, moved) ---- */

TEST_CASE("On unavailable as the requested mode: returns RADIO_MUX_REFUSE_BAD_VALUE", "[radio_mux]")
{
    radio_mux_rules_t none = no_rules();
    radio_mux_chan_view_t c[N_CHANS];

    three_channels(c);
    TEST_ASSERT_EQUAL(RADIO_MUX_REFUSE_BAD_VALUE,
                      radio_mux_orch_check_bind(&none, c, N_CHANS, 1, RADIO_MUX_MODE_UNAVAILABLE, false, NULL));
    TEST_ASSERT_EQUAL(RADIO_MUX_REFUSE_BAD_VALUE,
                      radio_mux_orch_check_bind(&none, c, N_CHANS, 1, RADIO_MUX_MODE_INVALID, false, NULL));
}

TEST_CASE("On any mode for an unavailable channel: returns RADIO_MUX_REFUSE_UNAVAILABLE", "[radio_mux]")
{
    radio_mux_rules_t none = no_rules();
    radio_mux_chan_view_t c[N_CHANS];

    three_channels(c);
    c[1].present = false;
    TEST_ASSERT_EQUAL(RADIO_MUX_REFUSE_UNAVAILABLE,
                      radio_mux_orch_check_bind(&none, c, N_CHANS, 1, RADIO_MUX_MODE_RCP_PROXY, false, NULL));
    TEST_ASSERT_EQUAL(RADIO_MUX_REFUSE_UNAVAILABLE,
                      radio_mux_orch_check_bind(&none, c, N_CHANS, 1, RADIO_MUX_MODE_OFF, false, NULL));
    c[1].present = true;
    c[1].supported = false;
    TEST_ASSERT_EQUAL(RADIO_MUX_REFUSE_UNAVAILABLE,
                      radio_mux_orch_check_bind(&none, c, N_CHANS, 1, RADIO_MUX_MODE_OTBR, false, NULL));
}

TEST_CASE("On a mode with no registered consumer (ziggurat in this build): returns RADIO_MUX_REFUSE_NOT_AVAILABLE",
          "[radio_mux]")
{
    radio_mux_rules_t none = no_rules();
    radio_mux_chan_view_t c[N_CHANS];

    three_channels(c);
    TEST_ASSERT_EQUAL(RADIO_MUX_REFUSE_NOT_AVAILABLE,
                      radio_mux_orch_check_bind(&none, c, N_CHANS, 1, MODE_ZIGGURAT, false, NULL));
}

TEST_CASE("On a mode the detected firmware doesn't fit: returns RADIO_MUX_REFUSE_INCOMPATIBLE", "[radio_mux]")
{
    radio_mux_rules_t zig = ziggurat_rules();
    radio_mux_chan_view_t c[N_CHANS];

    three_channels(c);
    TEST_ASSERT_EQUAL(RADIO_MUX_REFUSE_INCOMPATIBLE,
                      radio_mux_orch_check_bind(&zig, c, N_CHANS, 2, RADIO_MUX_MODE_OTBR, false, NULL));
    TEST_ASSERT_EQUAL(RADIO_MUX_REFUSE_INCOMPATIBLE,
                      radio_mux_orch_check_bind(&zig, c, N_CHANS, 2, MODE_ZIGGURAT, false, NULL));
    c[2].fw = RADIO_MUX_FW_UNKNOWN;
    TEST_ASSERT_EQUAL(RADIO_MUX_REFUSE_INCOMPATIBLE,
                      radio_mux_orch_check_bind(&zig, c, N_CHANS, 2, RADIO_MUX_MODE_OTBR, false, NULL));
}

TEST_CASE("On otbr while another channel is otbr: returns RADIO_MUX_MOVE with the other channel as the source",
          "[radio_mux]")
{
    radio_mux_rules_t none = no_rules();
    radio_mux_chan_view_t c[N_CHANS];
    size_t from = 99;

    three_channels(c);
    TEST_ASSERT_EQUAL(RADIO_MUX_MOVE, radio_mux_orch_check_bind(&none, c, N_CHANS, 1, RADIO_MUX_MODE_OTBR, false, &from));
    TEST_ASSERT_EQUAL(0, from);
    // The same source when the caller does not ask for it.
    TEST_ASSERT_EQUAL(RADIO_MUX_MOVE, radio_mux_orch_check_bind(&none, c, N_CHANS, 1, RADIO_MUX_MODE_OTBR, false, NULL));
    // Moving back: the source is the dongle.
    c[0].mode = RADIO_MUX_MODE_OFF;
    c[1].mode = RADIO_MUX_MODE_OTBR;
    TEST_ASSERT_EQUAL(RADIO_MUX_MOVE, radio_mux_orch_check_bind(&none, c, N_CHANS, 0, RADIO_MUX_MODE_OTBR, false, &from));
    TEST_ASSERT_EQUAL(1, from);
}

TEST_CASE("On otbr for the channel that already is otbr: returns RADIO_MUX_APPLY", "[radio_mux]")
{
    radio_mux_rules_t none = no_rules();
    radio_mux_chan_view_t c[N_CHANS];
    size_t from = 99;

    three_channels(c);
    TEST_ASSERT_EQUAL(RADIO_MUX_APPLY, radio_mux_orch_check_bind(&none, c, N_CHANS, 0, RADIO_MUX_MODE_OTBR, false, &from));
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
                      radio_mux_orch_check_bind(&zig, c, N_CHANS, 2, MODE_ZIGGURAT, false, NULL));
    // The channel that has the mode may set it again, and a higher limit lets a second channel in.
    TEST_ASSERT_EQUAL(RADIO_MUX_APPLY, radio_mux_orch_check_bind(&zig, c, N_CHANS, 1, MODE_ZIGGURAT, false, NULL));
    zig.rules[0].limit = 2;
    TEST_ASSERT_EQUAL(RADIO_MUX_APPLY, radio_mux_orch_check_bind(&zig, c, N_CHANS, 2, MODE_ZIGGURAT, false, NULL));
    zig.rules[0].limit = 0;
    TEST_ASSERT_EQUAL(RADIO_MUX_APPLY, radio_mux_orch_check_bind(&zig, c, N_CHANS, 2, MODE_ZIGGURAT, false, NULL));
}

TEST_CASE("On otbr while a ziggurat consumer is active, or ziggurat while otbr is active: returns "
          "RADIO_MUX_REFUSE_HYBRID", "[radio_mux]")
{
    radio_mux_rules_t zig = ziggurat_rules();
    radio_mux_chan_view_t c[N_CHANS];

    // ziggurat requested on the dongle while the c6 is otbr
    three_channels(c);
    TEST_ASSERT_EQUAL(RADIO_MUX_REFUSE_HYBRID, radio_mux_orch_check_bind(&zig, c, N_CHANS, 1, MODE_ZIGGURAT, false, NULL));
    // otbr requested on the c6 while the dongle runs ziggurat
    c[0].mode = RADIO_MUX_MODE_OFF;
    c[1].mode = MODE_ZIGGURAT;
    TEST_ASSERT_EQUAL(RADIO_MUX_REFUSE_HYBRID, radio_mux_orch_check_bind(&zig, c, N_CHANS, 0, RADIO_MUX_MODE_OTBR, false, NULL));
    // A channel leaving ziggurat for otbr is not a hybrid: nothing else runs ziggurat (a plain start of otbr).
    TEST_ASSERT_EQUAL(RADIO_MUX_MOVE, radio_mux_orch_check_bind(&zig, c, N_CHANS, 1, RADIO_MUX_MODE_OTBR, false, NULL));
}

TEST_CASE("On otbr while another channel stores ziggurat but no ziggurat consumer is registered: not a hybrid",
          "[radio_mux]")
{
    radio_mux_rules_t none = no_rules();
    radio_mux_chan_view_t c[N_CHANS];
    size_t from = 99;

    three_channels(c);
    c[0].mode = RADIO_MUX_MODE_OFF;
    c[1].mode = MODE_ZIGGURAT; // stored by a build that had the consumer
    TEST_ASSERT_EQUAL(RADIO_MUX_MOVE, radio_mux_orch_check_bind(&none, c, N_CHANS, 0, RADIO_MUX_MODE_OTBR, false, &from));
    TEST_ASSERT_EQUAL(N_CHANS, from);
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
    TEST_ASSERT_EQUAL(RADIO_MUX_APPLY_REBOOT, radio_mux_orch_check_bind(&zig, c, N_CHANS, 1, MODE_ZIGGURAT, false, NULL));
    // leave
    c[1].mode = MODE_ZIGGURAT;
    TEST_ASSERT_EQUAL(RADIO_MUX_APPLY_REBOOT, radio_mux_orch_check_bind(&zig, c, N_CHANS, 1, RADIO_MUX_MODE_OFF, false, NULL));
    TEST_ASSERT_EQUAL(RADIO_MUX_APPLY_REBOOT,
                      radio_mux_orch_check_bind(&zig, c, N_CHANS, 1, RADIO_MUX_MODE_RCP_PROXY, false, NULL));
}

TEST_CASE("On off or rcp-proxy otherwise: returns RADIO_MUX_APPLY", "[radio_mux]")
{
    radio_mux_rules_t none = no_rules();
    radio_mux_rules_t zig = ziggurat_rules();
    radio_mux_chan_view_t c[N_CHANS];

    three_channels(c);
    // Any firmware, including one otbr does not fit.
    TEST_ASSERT_EQUAL(RADIO_MUX_APPLY, radio_mux_orch_check_bind(&none, c, N_CHANS, 2, RADIO_MUX_MODE_RCP_PROXY, false, NULL));
    TEST_ASSERT_EQUAL(RADIO_MUX_APPLY, radio_mux_orch_check_bind(&none, c, N_CHANS, 2, RADIO_MUX_MODE_OFF, false, NULL));
    // Leaving a registered mode without a reboot declaration.
    c[0].mode = RADIO_MUX_MODE_OFF;
    c[1].mode = MODE_ZIGGURAT;
    TEST_ASSERT_EQUAL(RADIO_MUX_APPLY, radio_mux_orch_check_bind(&zig, c, N_CHANS, 1, RADIO_MUX_MODE_OFF, false, NULL));
}

TEST_CASE("On otbr for a channel while no channel has otbr: returns RADIO_MUX_MOVE from no source (a plain start)",
          "[radio_mux]")
{
    radio_mux_rules_t none = no_rules();
    radio_mux_chan_view_t c[N_CHANS];
    size_t from = 99;

    three_channels(c);
    c[0].mode = RADIO_MUX_MODE_OFF;
    TEST_ASSERT_EQUAL(RADIO_MUX_MOVE, radio_mux_orch_check_bind(&none, c, N_CHANS, 1, RADIO_MUX_MODE_OTBR, false, &from));
    TEST_ASSERT_EQUAL(N_CHANS, from); // n: no channel to move from
    c[1].mode = RADIO_MUX_MODE_RCP_PROXY; // from another mode the same (the lease refuses a running consumer)
    TEST_ASSERT_EQUAL(RADIO_MUX_MOVE, radio_mux_orch_check_bind(&none, c, N_CHANS, 1, RADIO_MUX_MODE_OTBR, false, &from));
    TEST_ASSERT_EQUAL(N_CHANS, from);
}

TEST_CASE("On off or rcp-proxy for the channel that has otbr: returns RADIO_MUX_MOVE with itself as the source "
          "(a plain stop)", "[radio_mux]")
{
    radio_mux_rules_t none = no_rules();
    radio_mux_chan_view_t c[N_CHANS];
    size_t from = 99;

    three_channels(c);
    TEST_ASSERT_EQUAL(RADIO_MUX_MOVE, radio_mux_orch_check_bind(&none, c, N_CHANS, 0, RADIO_MUX_MODE_OFF, false, &from));
    TEST_ASSERT_EQUAL(0, from);
    from = 99;
    TEST_ASSERT_EQUAL(RADIO_MUX_MOVE, radio_mux_orch_check_bind(&none, c, N_CHANS, 0, RADIO_MUX_MODE_RCP_PROXY, false, &from));
    TEST_ASSERT_EQUAL(0, from);
}

TEST_CASE("On leaving otbr for a mode whose registration reboots: returns RADIO_MUX_APPLY_REBOOT, not a stop",
          "[radio_mux]")
{
    radio_mux_rules_t zig = ziggurat_rules();
    radio_mux_chan_view_t c[N_CHANS];

    zig.rules[0].reboot_on_change = true;
    three_channels(c);
    TEST_ASSERT_EQUAL(RADIO_MUX_APPLY_REBOOT, radio_mux_orch_check_bind(&zig, c, N_CHANS, 0, MODE_ZIGGURAT, false, NULL));
}

TEST_CASE("On otbr for a channel leaving a mode whose registration reboots: returns RADIO_MUX_APPLY_REBOOT, not a start",
          "[radio_mux]")
{
    radio_mux_rules_t zig = ziggurat_rules();
    radio_mux_chan_view_t c[N_CHANS];
    size_t from = 99;

    zig.rules[0].reboot_on_change = true;
    three_channels(c);
    c[0].mode = RADIO_MUX_MODE_OFF;
    c[1].mode = MODE_ZIGGURAT;
    TEST_ASSERT_EQUAL(RADIO_MUX_APPLY_REBOOT, radio_mux_orch_check_bind(&zig, c, N_CHANS, 1, RADIO_MUX_MODE_OTBR, false, &from));
    TEST_ASSERT_EQUAL(99, from); // no move: nothing probes the channel the consumer still holds
}

TEST_CASE("On otbr for a channel leaving a mode that does not reboot: still RADIO_MUX_MOVE from no source (a start)",
          "[radio_mux]")
{
    radio_mux_rules_t zig = ziggurat_rules();
    radio_mux_chan_view_t c[N_CHANS];
    size_t from = 99;

    three_channels(c);
    c[0].mode = RADIO_MUX_MODE_OFF;
    c[1].mode = MODE_ZIGGURAT;
    TEST_ASSERT_EQUAL(RADIO_MUX_MOVE, radio_mux_orch_check_bind(&zig, c, N_CHANS, 1, RADIO_MUX_MODE_OTBR, false, &from));
    TEST_ASSERT_EQUAL(N_CHANS, from);
}

TEST_CASE("On a change on a channel in reinit: returns RADIO_MUX_REFUSE_BUSY", "[radio_mux]")
{
    radio_mux_rules_t none = no_rules();
    radio_mux_chan_view_t c[N_CHANS];
    size_t from = 99;

    three_channels(c);
    TEST_ASSERT_EQUAL(RADIO_MUX_REFUSE_BUSY,
                      radio_mux_orch_check_bind(&none, c, N_CHANS, 1, RADIO_MUX_MODE_RCP_PROXY, true, NULL));
    TEST_ASSERT_EQUAL(RADIO_MUX_REFUSE_BUSY,
                      radio_mux_orch_check_bind(&none, c, N_CHANS, 0, RADIO_MUX_MODE_OFF, true, &from));
    TEST_ASSERT_EQUAL(99, from); // no move
    // The same mode again is a change too (a PUT of the mode retries a fault).
    TEST_ASSERT_EQUAL(RADIO_MUX_REFUSE_BUSY,
                      radio_mux_orch_check_bind(&none, c, N_CHANS, 0, RADIO_MUX_MODE_OTBR, true, NULL));
}

TEST_CASE("On a bad value for a channel in reinit: the bad value wins over busy", "[radio_mux]")
{
    radio_mux_rules_t none = no_rules();
    radio_mux_chan_view_t c[N_CHANS];

    three_channels(c);
    TEST_ASSERT_EQUAL(RADIO_MUX_REFUSE_BAD_VALUE,
                      radio_mux_orch_check_bind(&none, c, N_CHANS, 1, RADIO_MUX_MODE_INVALID, true, NULL));
}

/* ---- radio_mux_orch_should_bind ---- */

TEST_CASE("On a ready channel whose firmware fits and no instance bound: binds", "[radio_mux]")
{
    TEST_ASSERT_TRUE(radio_mux_orch_should_bind(RADIO_MUX_CS_READY, true, false));
}

TEST_CASE("On a ready channel whose firmware no longer fits: does not bind", "[radio_mux]")
{
    TEST_ASSERT_FALSE(radio_mux_orch_should_bind(RADIO_MUX_CS_READY, false, false));
}

TEST_CASE("On a ready channel whose instance is already bound: does not bind again", "[radio_mux]")
{
    TEST_ASSERT_FALSE(radio_mux_orch_should_bind(RADIO_MUX_CS_READY, true, true));
}

TEST_CASE("On a channel absent, in reinit or in fault: does not bind", "[radio_mux]")
{
    TEST_ASSERT_FALSE(radio_mux_orch_should_bind(RADIO_MUX_CS_ABSENT, true, false));
    TEST_ASSERT_FALSE(radio_mux_orch_should_bind(RADIO_MUX_CS_REINIT, true, false));
    TEST_ASSERT_FALSE(radio_mux_orch_should_bind(RADIO_MUX_CS_FAULT, true, false));
}

/* ---- radio_mux_singleton_dedupe (today's radio_mux_otbr_dedupe, extended) ---- */

TEST_CASE("On one otbr channel: nothing changes", "[radio_mux]")
{
    radio_mux_mode_t m[3] = {RADIO_MUX_MODE_OFF, RADIO_MUX_MODE_OTBR, RADIO_MUX_MODE_RCP_PROXY};
    bool changed[3] = {true, true, true};

    TEST_ASSERT_EQUAL_UINT32(0, radio_mux_singleton_dedupe(m, changed, 3));
    TEST_ASSERT_EQUAL(RADIO_MUX_MODE_OTBR, m[1]);
    TEST_ASSERT_FALSE(changed[0] || changed[1] || changed[2]);
}

TEST_CASE("On the C6 and a dongle both stored otbr: the C6 (slot 0) keeps it, the dongle becomes off", "[radio_mux]")
{
    radio_mux_mode_t m[3] = {RADIO_MUX_MODE_OTBR, RADIO_MUX_MODE_OTBR, RADIO_MUX_MODE_OFF};
    bool changed[3];

    TEST_ASSERT_EQUAL_UINT32(1, radio_mux_singleton_dedupe(m, changed, 3));
    TEST_ASSERT_EQUAL(RADIO_MUX_MODE_OTBR, m[0]);
    TEST_ASSERT_EQUAL(RADIO_MUX_MODE_OFF, m[1]);
    TEST_ASSERT_FALSE(changed[0]);
    TEST_ASSERT_TRUE(changed[1]);
    TEST_ASSERT_FALSE(changed[2]);
}

TEST_CASE("On two dongles stored otbr: the first in slot order keeps it", "[radio_mux]")
{
    radio_mux_mode_t m[4] = {RADIO_MUX_MODE_OFF, RADIO_MUX_MODE_RCP_PROXY, RADIO_MUX_MODE_OTBR, RADIO_MUX_MODE_OTBR};
    bool changed[4];

    TEST_ASSERT_EQUAL_UINT32(1, radio_mux_singleton_dedupe(m, changed, 4));
    TEST_ASSERT_EQUAL(RADIO_MUX_MODE_OTBR, m[2]);
    TEST_ASSERT_EQUAL(RADIO_MUX_MODE_OFF, m[3]);
    TEST_ASSERT_EQUAL(RADIO_MUX_MODE_RCP_PROXY, m[1]);
    TEST_ASSERT_TRUE(changed[3]);
}

TEST_CASE("On no otbr channel: nothing changes", "[radio_mux]")
{
    radio_mux_mode_t m[2] = {RADIO_MUX_MODE_OFF, RADIO_MUX_MODE_OFF};
    bool changed[2];

    TEST_ASSERT_EQUAL_UINT32(0, radio_mux_singleton_dedupe(m, changed, 2));
}

TEST_CASE("On two channels stored ziggurat: the first in slot order keeps it, the other becomes off", "[radio_mux]")
{
    radio_mux_mode_t m[4] = {RADIO_MUX_MODE_OFF, MODE_ZIGGURAT, RADIO_MUX_MODE_RCP_PROXY, MODE_ZIGGURAT};
    bool changed[4];

    TEST_ASSERT_EQUAL_UINT32(1, radio_mux_singleton_dedupe(m, changed, 4));
    TEST_ASSERT_EQUAL(MODE_ZIGGURAT, m[1]);
    TEST_ASSERT_EQUAL(RADIO_MUX_MODE_OFF, m[3]);
    TEST_ASSERT_EQUAL(RADIO_MUX_MODE_RCP_PROXY, m[2]);
    TEST_ASSERT_FALSE(changed[1]);
    TEST_ASSERT_TRUE(changed[3]);
}

TEST_CASE("On two otbr and two ziggurat: each keeps its first, both repeats become off", "[radio_mux]")
{
    radio_mux_mode_t m[4] = {MODE_ZIGGURAT, RADIO_MUX_MODE_OTBR, MODE_ZIGGURAT, RADIO_MUX_MODE_OTBR};
    bool changed[4];

    TEST_ASSERT_EQUAL_UINT32(2, radio_mux_singleton_dedupe(m, changed, 4));
    TEST_ASSERT_EQUAL(MODE_ZIGGURAT, m[0]);
    TEST_ASSERT_EQUAL(RADIO_MUX_MODE_OTBR, m[1]);
    TEST_ASSERT_EQUAL(RADIO_MUX_MODE_OFF, m[2]);
    TEST_ASSERT_EQUAL(RADIO_MUX_MODE_OFF, m[3]);
    TEST_ASSERT_TRUE(changed[2] && changed[3]);
    TEST_ASSERT_FALSE(changed[0] || changed[1]);
}

TEST_CASE("On two channels stored rcp-proxy: both keep it (one proxy instance per channel)", "[radio_mux]")
{
    radio_mux_mode_t m[2] = {RADIO_MUX_MODE_RCP_PROXY, RADIO_MUX_MODE_RCP_PROXY};
    bool changed[2];

    TEST_ASSERT_EQUAL_UINT32(0, radio_mux_singleton_dedupe(m, changed, 2));
    TEST_ASSERT_EQUAL(RADIO_MUX_MODE_RCP_PROXY, m[1]);
}

/* ---- the reinit run: radio_mux_reinit_ask / _answer / _tick / _done ---- */

TEST_CASE("On a reinit asked of a channel with no instance bound: runs at once", "[radio_mux]")
{
    radio_mux_reinit_run_t r = {0};

    TEST_ASSERT_EQUAL(RADIO_MUX_RA_RUN, radio_mux_reinit_ask(&r, 7, false, NOW));
    TEST_ASSERT_EQUAL(RADIO_MUX_RP_RUNNING, r.phase);
}

TEST_CASE("On a reinit asked of a bound instance: waits for its answer until RADIO_MUX_REINIT_REPLY_MS",
          "[radio_mux]")
{
    radio_mux_reinit_run_t r = {0};

    TEST_ASSERT_EQUAL(RADIO_MUX_RA_WAIT, radio_mux_reinit_ask(&r, 7, true, NOW));
    TEST_ASSERT_EQUAL(RADIO_MUX_RP_ASKED, r.phase);
    TEST_ASSERT_EQUAL_UINT32(7, r.seq);
    TEST_ASSERT_EQUAL_INT64(NOW + RADIO_MUX_REINIT_REPLY_MS, r.deadline_ms);
    TEST_ASSERT_EQUAL(50000, RADIO_MUX_REINIT_REPLY_MS);
}

TEST_CASE("On a bound instance that accepts with the current seq: runs", "[radio_mux]")
{
    radio_mux_reinit_run_t r = {0};

    radio_mux_reinit_ask(&r, 7, true, NOW);
    TEST_ASSERT_EQUAL(RADIO_MUX_RA_RUN, radio_mux_reinit_answer(&r, 7, true));
    TEST_ASSERT_EQUAL(RADIO_MUX_RP_RUNNING, r.phase);
}

TEST_CASE("On a bound instance that refuses: refuses, phase back to idle", "[radio_mux]")
{
    radio_mux_reinit_run_t r = {0};

    radio_mux_reinit_ask(&r, 7, true, NOW);
    TEST_ASSERT_EQUAL(RADIO_MUX_RA_REFUSE, radio_mux_reinit_answer(&r, 7, false));
    TEST_ASSERT_EQUAL(RADIO_MUX_RP_IDLE, r.phase);
}

TEST_CASE("On an answer with an older seq: is ignored", "[radio_mux]")
{
    radio_mux_reinit_run_t r = {0};

    radio_mux_reinit_ask(&r, 7, true, NOW);
    TEST_ASSERT_EQUAL(RADIO_MUX_RA_IGNORE, radio_mux_reinit_answer(&r, 6, true));
    TEST_ASSERT_EQUAL(RADIO_MUX_RA_IGNORE, radio_mux_reinit_answer(&r, 6, false));
    TEST_ASSERT_EQUAL(RADIO_MUX_RP_ASKED, r.phase);
}

TEST_CASE("On an answer while nothing was asked: is ignored", "[radio_mux]")
{
    radio_mux_reinit_run_t r = {0};

    TEST_ASSERT_EQUAL(RADIO_MUX_RA_IGNORE, radio_mux_reinit_answer(&r, 0, true));
    TEST_ASSERT_EQUAL(RADIO_MUX_RP_IDLE, r.phase);
    radio_mux_reinit_ask(&r, 7, false, NOW); // running: a late answer changes nothing
    TEST_ASSERT_EQUAL(RADIO_MUX_RA_IGNORE, radio_mux_reinit_answer(&r, 7, false));
    TEST_ASSERT_EQUAL(RADIO_MUX_RP_RUNNING, r.phase);
}

TEST_CASE("On no answer within RADIO_MUX_REINIT_REPLY_MS: refuses (timeout), phase back to idle", "[radio_mux]")
{
    radio_mux_reinit_run_t r = {0};

    radio_mux_reinit_ask(&r, 7, true, NOW);
    TEST_ASSERT_EQUAL(RADIO_MUX_RA_WAIT, radio_mux_reinit_tick(&r, NOW + RADIO_MUX_REINIT_REPLY_MS - 1));
    TEST_ASSERT_EQUAL(RADIO_MUX_RP_ASKED, r.phase);
    TEST_ASSERT_EQUAL(RADIO_MUX_RA_REFUSE, radio_mux_reinit_tick(&r, NOW + RADIO_MUX_REINIT_REPLY_MS));
    TEST_ASSERT_EQUAL(RADIO_MUX_RP_IDLE, r.phase);
    // A refusal that comes after the timeout changes nothing.
    TEST_ASSERT_EQUAL(RADIO_MUX_RA_IGNORE, radio_mux_reinit_answer(&r, 7, false));
    TEST_ASSERT_EQUAL(RADIO_MUX_RP_IDLE, r.phase);
}

TEST_CASE("On an accept that comes after the timeout: rebinds once, phase stays idle", "[radio_mux]")
{
    radio_mux_reinit_run_t r = {0};

    radio_mux_reinit_ask(&r, 7, true, NOW);
    radio_mux_reinit_tick(&r, NOW + RADIO_MUX_REINIT_REPLY_MS);
    // The instance let go for a maintenance that no longer runs: it gets its channel back.
    TEST_ASSERT_EQUAL(RADIO_MUX_RA_REBIND, radio_mux_reinit_answer(&r, 7, true));
    TEST_ASSERT_EQUAL(RADIO_MUX_RP_IDLE, r.phase);
    TEST_ASSERT_EQUAL(RADIO_MUX_RA_IGNORE, radio_mux_reinit_answer(&r, 7, true));
}

TEST_CASE("On a late accept of an older seq, or after a new run was asked: is ignored", "[radio_mux]")
{
    radio_mux_reinit_run_t r = {0};

    radio_mux_reinit_ask(&r, 7, true, NOW);
    radio_mux_reinit_tick(&r, NOW + RADIO_MUX_REINIT_REPLY_MS);
    TEST_ASSERT_EQUAL(RADIO_MUX_RA_IGNORE, radio_mux_reinit_answer(&r, 6, true));
    // A new run asks with seq 8: its own end binds the instance back.
    radio_mux_reinit_ask(&r, 8, true, NOW + RADIO_MUX_REINIT_REPLY_MS + 1);
    TEST_ASSERT_EQUAL(RADIO_MUX_RA_IGNORE, radio_mux_reinit_answer(&r, 7, true));
    TEST_ASSERT_EQUAL(RADIO_MUX_RP_ASKED, r.phase);
}

TEST_CASE("On an accept after a refusal or a finished run: is ignored", "[radio_mux]")
{
    radio_mux_reinit_run_t r = {0};

    radio_mux_reinit_ask(&r, 7, true, NOW);
    radio_mux_reinit_answer(&r, 7, false);
    TEST_ASSERT_EQUAL(RADIO_MUX_RA_IGNORE, radio_mux_reinit_answer(&r, 7, true));
    radio_mux_reinit_ask(&r, 8, true, NOW);
    radio_mux_reinit_answer(&r, 8, true);
    radio_mux_reinit_done(&r);
    TEST_ASSERT_EQUAL(RADIO_MUX_RA_IGNORE, radio_mux_reinit_answer(&r, 8, true));
}

TEST_CASE("On a tick while nothing waits for an answer: is ignored", "[radio_mux]")
{
    radio_mux_reinit_run_t r = {0};

    TEST_ASSERT_EQUAL(RADIO_MUX_RA_IGNORE, radio_mux_reinit_tick(&r, NOW));
    radio_mux_reinit_ask(&r, 7, false, NOW);
    TEST_ASSERT_EQUAL(RADIO_MUX_RA_IGNORE, radio_mux_reinit_tick(&r, NOW + 10 * RADIO_MUX_REINIT_REPLY_MS));
    TEST_ASSERT_EQUAL(RADIO_MUX_RP_RUNNING, r.phase);
}

TEST_CASE("On a reinit asked while another one is asked or running: refuses, the run unchanged", "[radio_mux]")
{
    radio_mux_reinit_run_t r = {0};

    radio_mux_reinit_ask(&r, 7, true, NOW);
    TEST_ASSERT_EQUAL(RADIO_MUX_RA_REFUSE, radio_mux_reinit_ask(&r, 8, true, NOW + 1));
    TEST_ASSERT_EQUAL(RADIO_MUX_RP_ASKED, r.phase);
    TEST_ASSERT_EQUAL_UINT32(7, r.seq);
    radio_mux_reinit_answer(&r, 7, true);
    TEST_ASSERT_EQUAL(RADIO_MUX_RA_REFUSE, radio_mux_reinit_ask(&r, 8, false, NOW + 2));
    TEST_ASSERT_EQUAL(RADIO_MUX_RP_RUNNING, r.phase);
}

TEST_CASE("On done: phase back to idle", "[radio_mux]")
{
    radio_mux_reinit_run_t r = {0};

    radio_mux_reinit_ask(&r, 7, false, NOW);
    radio_mux_reinit_done(&r);
    TEST_ASSERT_EQUAL(RADIO_MUX_RP_IDLE, r.phase);
    TEST_ASSERT_EQUAL(RADIO_MUX_RA_RUN, radio_mux_reinit_ask(&r, 8, false, NOW + 1)); // a new run may start
}

/* ---- texts ---- */

TEST_CASE("On each channel state: returns its wire name", "[radio_mux]")
{
    TEST_ASSERT_EQUAL_STRING("absent", radio_mux_chan_state_name(RADIO_MUX_CS_ABSENT));
    TEST_ASSERT_EQUAL_STRING("reinit", radio_mux_chan_state_name(RADIO_MUX_CS_REINIT));
    TEST_ASSERT_EQUAL_STRING("ready", radio_mux_chan_state_name(RADIO_MUX_CS_READY));
    TEST_ASSERT_EQUAL_STRING("fault", radio_mux_chan_state_name(RADIO_MUX_CS_FAULT));
    TEST_ASSERT_NULL(radio_mux_chan_state_name((radio_mux_chan_state_t)99));
}

TEST_CASE("On each maintenance work: returns the reinit reason of the spec", "[radio_mux]")
{
    TEST_ASSERT_EQUAL_STRING("boot steps", radio_mux_work_reason(RADIO_MUX_WORK_BOOT_STEPS));
    TEST_ASSERT_EQUAL_STRING("detecting firmware", radio_mux_work_reason(RADIO_MUX_WORK_DETECT));
    TEST_ASSERT_EQUAL_STRING("firmware push", radio_mux_work_reason(RADIO_MUX_WORK_PUSH));
    TEST_ASSERT_EQUAL_STRING("ROM recovery", radio_mux_work_reason(RADIO_MUX_WORK_ROM_RECOVERY));
    TEST_ASSERT_EQUAL_STRING("", radio_mux_work_reason(RADIO_MUX_WORK_NONE));
    TEST_ASSERT_EQUAL_STRING("", radio_mux_work_reason((radio_mux_work_t)99));
}

TEST_CASE("On each maintenance work: returns today's busy text of the channel", "[radio_mux]")
{
    TEST_ASSERT_EQUAL_STRING("the C6's boot steps run", radio_mux_busy_reason(RADIO_MUX_WORK_BOOT_STEPS));
    TEST_ASSERT_EQUAL_STRING("a C6 job holds the radio", radio_mux_busy_reason(RADIO_MUX_WORK_PUSH));
    TEST_ASSERT_EQUAL_STRING("a C6 job holds the radio", radio_mux_busy_reason(RADIO_MUX_WORK_ROM_RECOVERY));
    TEST_ASSERT_EQUAL_STRING("", radio_mux_busy_reason(RADIO_MUX_WORK_DETECT));
    TEST_ASSERT_EQUAL_STRING("", radio_mux_busy_reason(RADIO_MUX_WORK_NONE));
}

/* ---- radio_mux_orch_input_for: what the binding loop delivers on a channel's change ---- */

TEST_CASE("On a ready channel with a fitting stored mode: delivers BIND", "[radio_mux]")
{
    // At boot (reinit: boot steps or detection), after a plug-in, and on a ready channel (a user's bind).
    TEST_ASSERT_EQUAL(RADIO_MUX_IN_BIND, radio_mux_orch_input_for(RADIO_MUX_CS_REINIT, RADIO_MUX_CS_READY, true, false));
    TEST_ASSERT_EQUAL(RADIO_MUX_IN_BIND, radio_mux_orch_input_for(RADIO_MUX_CS_ABSENT, RADIO_MUX_CS_READY, true, false));
    TEST_ASSERT_EQUAL(RADIO_MUX_IN_BIND, radio_mux_orch_input_for(RADIO_MUX_CS_READY, RADIO_MUX_CS_READY, true, false));
}

TEST_CASE("On an instance already running on the ready channel: delivers nothing", "[radio_mux]")
{
    TEST_ASSERT_EQUAL(RADIO_MUX_IN_NONE, radio_mux_orch_input_for(RADIO_MUX_CS_READY, RADIO_MUX_CS_READY, true, true));
}

TEST_CASE("On an unplug or a link/firmware fault while bound: delivers LOST", "[radio_mux]")
{
    TEST_ASSERT_EQUAL(RADIO_MUX_IN_LOST, radio_mux_orch_input_for(RADIO_MUX_CS_READY, RADIO_MUX_CS_ABSENT, true, true));
    TEST_ASSERT_EQUAL(RADIO_MUX_IN_LOST, radio_mux_orch_input_for(RADIO_MUX_CS_READY, RADIO_MUX_CS_FAULT, true, true));
}

TEST_CASE("On a channel lost with no instance bound, or lost again: delivers nothing", "[radio_mux]")
{
    TEST_ASSERT_EQUAL(RADIO_MUX_IN_NONE, radio_mux_orch_input_for(RADIO_MUX_CS_READY, RADIO_MUX_CS_ABSENT, true, false));
    TEST_ASSERT_EQUAL(RADIO_MUX_IN_NONE, radio_mux_orch_input_for(RADIO_MUX_CS_ABSENT, RADIO_MUX_CS_FAULT, true, true));
}

TEST_CASE("On the channel's return (detection done, fits): delivers BACK", "[radio_mux]")
{
    TEST_ASSERT_EQUAL(RADIO_MUX_IN_BACK, radio_mux_orch_input_for(RADIO_MUX_CS_REINIT, RADIO_MUX_CS_READY, true, true));
    TEST_ASSERT_EQUAL(RADIO_MUX_IN_BACK, radio_mux_orch_input_for(RADIO_MUX_CS_ABSENT, RADIO_MUX_CS_READY, true, true));
    TEST_ASSERT_EQUAL(RADIO_MUX_IN_BACK, radio_mux_orch_input_for(RADIO_MUX_CS_FAULT, RADIO_MUX_CS_READY, true, true));
}

TEST_CASE("On the return with firmware that no longer fits: delivers nothing (the binding is kept)", "[radio_mux]")
{
    TEST_ASSERT_EQUAL(RADIO_MUX_IN_NONE, radio_mux_orch_input_for(RADIO_MUX_CS_REINIT, RADIO_MUX_CS_READY, false, true));
    TEST_ASSERT_EQUAL(RADIO_MUX_IN_NONE, radio_mux_orch_input_for(RADIO_MUX_CS_ABSENT, RADIO_MUX_CS_READY, false, true));
    // Nor a first bind.
    TEST_ASSERT_EQUAL(RADIO_MUX_IN_NONE, radio_mux_orch_input_for(RADIO_MUX_CS_REINIT, RADIO_MUX_CS_READY, false, false));
}

TEST_CASE("On a ready channel entering reinit: delivers nothing (the maintenance asks with REINIT)", "[radio_mux]")
{
    TEST_ASSERT_EQUAL(RADIO_MUX_IN_NONE, radio_mux_orch_input_for(RADIO_MUX_CS_READY, RADIO_MUX_CS_REINIT, true, true));
    TEST_ASSERT_EQUAL(RADIO_MUX_IN_NONE, radio_mux_orch_input_for(RADIO_MUX_CS_ABSENT, RADIO_MUX_CS_REINIT, true, true));
}

TEST_CASE("On a PUT of another mode: the new mode's instance binds on the ready channel", "[radio_mux]")
{
    // The old instance got UNBIND (it is no longer bound); the new one is not bound yet.
    TEST_ASSERT_TRUE(radio_mux_orch_should_bind(RADIO_MUX_CS_READY, true, false));
    TEST_ASSERT_EQUAL(RADIO_MUX_IN_BIND, radio_mux_orch_input_for(RADIO_MUX_CS_READY, RADIO_MUX_CS_READY, true, false));
}

TEST_CASE("On the radio lost while it is in reinit (a fault, an unplug): delivers LOST, not BACK", "[radio_mux]")
{
    TEST_ASSERT_EQUAL(RADIO_MUX_IN_LOST, radio_mux_orch_input_for(RADIO_MUX_CS_REINIT, RADIO_MUX_CS_FAULT, true, true));
    TEST_ASSERT_EQUAL(RADIO_MUX_IN_LOST, radio_mux_orch_input_for(RADIO_MUX_CS_REINIT, RADIO_MUX_CS_ABSENT, true, true));
}

TEST_CASE("On LOST for a firmware fault: Thread's state is fault, Zigbee's and the proxy's no_radio", "[radio_mux]")
{
    TEST_ASSERT_EQUAL_STRING("fault", radio_mux_orch_lost_state(RADIO_MUX_MODE_OTBR, RADIO_MUX_FAULT_FIRMWARE));
    TEST_ASSERT_EQUAL_STRING("no_radio", radio_mux_orch_lost_state(RADIO_MUX_MODE_ZIGGURAT, RADIO_MUX_FAULT_FIRMWARE));
    TEST_ASSERT_EQUAL_STRING("no_radio", radio_mux_orch_lost_state(RADIO_MUX_MODE_RCP_PROXY, RADIO_MUX_FAULT_FIRMWARE));
}

TEST_CASE("On LOST for a link fault or an unplug: every mode's state is no_radio", "[radio_mux]")
{
    TEST_ASSERT_EQUAL_STRING("no_radio", radio_mux_orch_lost_state(RADIO_MUX_MODE_OTBR, RADIO_MUX_FAULT_LINK));
    TEST_ASSERT_EQUAL_STRING("no_radio", radio_mux_orch_lost_state(RADIO_MUX_MODE_OTBR, RADIO_MUX_FAULT_NONE));
    TEST_ASSERT_EQUAL_STRING("no_radio", radio_mux_orch_lost_state(RADIO_MUX_MODE_ZIGGURAT, RADIO_MUX_FAULT_LINK));
    TEST_ASSERT_EQUAL_STRING("no_radio", radio_mux_orch_lost_state(RADIO_MUX_MODE_RCP_PROXY, RADIO_MUX_FAULT_NONE));
}

/* ---- radio_mux_orch_settle: has the channel's mode run again after a maintenance ---- */

TEST_CASE("On no instance stored on the channel: settle is NONE", "[radio_mux]")
{
    TEST_ASSERT_EQUAL(RADIO_MUX_SETTLE_NONE, radio_mux_orch_settle(RADIO_MUX_CS_READY, false, ""));
    TEST_ASSERT_EQUAL(RADIO_MUX_SETTLE_NONE, radio_mux_orch_settle(RADIO_MUX_CS_FAULT, false, "no_radio"));
}

TEST_CASE("On the channel in fault or absent: settle is DOWN", "[radio_mux]")
{
    TEST_ASSERT_EQUAL(RADIO_MUX_SETTLE_DOWN, radio_mux_orch_settle(RADIO_MUX_CS_FAULT, true, "fault"));
    TEST_ASSERT_EQUAL(RADIO_MUX_SETTLE_DOWN, radio_mux_orch_settle(RADIO_MUX_CS_FAULT, true, "starting"));
    TEST_ASSERT_EQUAL(RADIO_MUX_SETTLE_DOWN, radio_mux_orch_settle(RADIO_MUX_CS_ABSENT, true, "running"));
}

TEST_CASE("On the channel still in reinit: settle is WAIT", "[radio_mux]")
{
    TEST_ASSERT_EQUAL(RADIO_MUX_SETTLE_WAIT, radio_mux_orch_settle(RADIO_MUX_CS_REINIT, true, "running"));
    TEST_ASSERT_EQUAL(RADIO_MUX_SETTLE_WAIT, radio_mux_orch_settle(RADIO_MUX_CS_REINIT, true, "no_radio"));
}

TEST_CASE("On a ready channel whose state is \"\", starting or releasing: settle is WAIT", "[radio_mux]")
{
    TEST_ASSERT_EQUAL(RADIO_MUX_SETTLE_WAIT, radio_mux_orch_settle(RADIO_MUX_CS_READY, true, ""));
    TEST_ASSERT_EQUAL(RADIO_MUX_SETTLE_WAIT, radio_mux_orch_settle(RADIO_MUX_CS_READY, true, "starting"));
    TEST_ASSERT_EQUAL(RADIO_MUX_SETTLE_WAIT, radio_mux_orch_settle(RADIO_MUX_CS_READY, true, "releasing"));
}

TEST_CASE("On a ready channel whose mode runs (running, sniffer, listening, connecting, backoff, connected): settle "
          "is UP", "[radio_mux]")
{
    static const char *const up[] = {"running", "sniffer", "listening", "connecting", "backoff", "connected"};

    for (size_t i = 0; i < sizeof(up) / sizeof(up[0]); i++) {
        TEST_ASSERT_EQUAL_MESSAGE(RADIO_MUX_SETTLE_UP, radio_mux_orch_settle(RADIO_MUX_CS_READY, true, up[i]), up[i]);
    }
}

TEST_CASE("On a ready channel whose mode does not run (fault, no_radio, incompatible, idle, off, anything else): "
          "settle is DOWN", "[radio_mux]")
{
    static const char *const down[] = {"fault", "no_radio", "incompatible", "idle", "off", "bogus"};

    for (size_t i = 0; i < sizeof(down) / sizeof(down[0]); i++) {
        TEST_ASSERT_EQUAL_MESSAGE(RADIO_MUX_SETTLE_DOWN, radio_mux_orch_settle(RADIO_MUX_CS_READY, true, down[i]),
                                  down[i]);
    }
}
