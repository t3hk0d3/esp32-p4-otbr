// The attach state machine, the attach decision (R13), the non-blocking RX post and the small pure helpers of the mux
// runtime.
#include <string.h>
#include "radio_mux_core.h"
#include "unity.h"

#define WANT true
#define NO_WANT false

static radio_mux_attach_t at(radio_mux_attach_state_t s)
{
    return (radio_mux_attach_t) {
        .state = s, .plug_pending = false
    };
}

// Runs one event and checks the action and the state it leads to.
static void step(radio_mux_attach_t *a, radio_mux_attach_ev_t ev, bool want, radio_mux_attach_act_t act,
                 radio_mux_attach_state_t next)
{
    TEST_ASSERT_EQUAL(act, radio_mux_attach_next(a, ev, want));
    TEST_ASSERT_EQUAL(next, a->state);
}

/* ---- plug-in ---- */

TEST_CASE("plug on an absent transport: probes in mode off", "[radio_mux]")
{
    radio_mux_attach_t a = at(RADIO_MUX_AS_ABSENT);

    step(&a, RADIO_MUX_AEV_PLUG, NO_WANT, RADIO_MUX_ACT_PROBE, RADIO_MUX_AS_PROBING);
}

TEST_CASE("detection done while probing: reopens the device", "[radio_mux]")
{
    radio_mux_attach_t a = at(RADIO_MUX_AS_PROBING);

    step(&a, RADIO_MUX_AEV_DETECT_DONE, NO_WANT, RADIO_MUX_ACT_REOPEN, RADIO_MUX_AS_REOPENING);
}

TEST_CASE("reopened with the stored mode's consumer wanted: attaches it", "[radio_mux]")
{
    radio_mux_attach_t a = at(RADIO_MUX_AS_REOPENING);

    step(&a, RADIO_MUX_AEV_REOPENED, WANT, RADIO_MUX_ACT_ATTACH, RADIO_MUX_AS_ATTACHED);
}

TEST_CASE("reopened with no consumer wanted: stays idle", "[radio_mux]")
{
    radio_mux_attach_t a = at(RADIO_MUX_AS_REOPENING);

    step(&a, RADIO_MUX_AEV_REOPENED, NO_WANT, RADIO_MUX_ACT_NONE, RADIO_MUX_AS_IDLE);
}

TEST_CASE("a supported device: off, detection, reopen, then the consumer attaches", "[radio_mux]")
{
    radio_mux_attach_t a = at(RADIO_MUX_AS_ABSENT);

    step(&a, RADIO_MUX_AEV_PLUG, NO_WANT, RADIO_MUX_ACT_PROBE, RADIO_MUX_AS_PROBING);
    step(&a, RADIO_MUX_AEV_DETECT_DONE, NO_WANT, RADIO_MUX_ACT_REOPEN, RADIO_MUX_AS_REOPENING);
    step(&a, RADIO_MUX_AEV_REOPENED, WANT, RADIO_MUX_ACT_ATTACH, RADIO_MUX_AS_ATTACHED);
}

TEST_CASE("attach failed: the transport is idle", "[radio_mux]")
{
    radio_mux_attach_t a = at(RADIO_MUX_AS_ATTACHED);

    step(&a, RADIO_MUX_AEV_ATTACH_FAILED, NO_WANT, RADIO_MUX_ACT_NONE, RADIO_MUX_AS_IDLE);
}

TEST_CASE("attach failed while not attached: changes nothing", "[radio_mux]")
{
    radio_mux_attach_t a = at(RADIO_MUX_AS_IDLE);

    step(&a, RADIO_MUX_AEV_ATTACH_FAILED, NO_WANT, RADIO_MUX_ACT_NONE, RADIO_MUX_AS_IDLE);
}

TEST_CASE("plug while a device is open: ignored", "[radio_mux]")
{
    const radio_mux_attach_state_t open[] = {
        RADIO_MUX_AS_PROBING, RADIO_MUX_AS_REOPENING, RADIO_MUX_AS_IDLE, RADIO_MUX_AS_ATTACHED,
    };

    for (size_t i = 0; i < sizeof(open) / sizeof(open[0]); i++) {
        radio_mux_attach_t a = at(open[i]);

        step(&a, RADIO_MUX_AEV_PLUG, WANT, RADIO_MUX_ACT_NONE, open[i]);
        TEST_ASSERT_FALSE(a.plug_pending);
    }
}

/* ---- mode change ---- */

TEST_CASE("mode set on an idle transport with the consumer wanted: attaches it", "[radio_mux]")
{
    radio_mux_attach_t a = at(RADIO_MUX_AS_IDLE);

    step(&a, RADIO_MUX_AEV_MODE_SET, WANT, RADIO_MUX_ACT_ATTACH, RADIO_MUX_AS_ATTACHED);
}

TEST_CASE("mode set on an idle transport with nothing wanted: stays idle", "[radio_mux]")
{
    radio_mux_attach_t a = at(RADIO_MUX_AS_IDLE);

    step(&a, RADIO_MUX_AEV_MODE_SET, NO_WANT, RADIO_MUX_ACT_NONE, RADIO_MUX_AS_IDLE);
}

TEST_CASE("mode set on an attached transport to another consumer: reattaches", "[radio_mux]")
{
    radio_mux_attach_t a = at(RADIO_MUX_AS_ATTACHED);

    step(&a, RADIO_MUX_AEV_MODE_SET, WANT, RADIO_MUX_ACT_REATTACH, RADIO_MUX_AS_ATTACHED);
}

TEST_CASE("mode set on an attached transport to a mode with no consumer: detaches", "[radio_mux]")
{
    radio_mux_attach_t a = at(RADIO_MUX_AS_ATTACHED);

    step(&a, RADIO_MUX_AEV_MODE_SET, NO_WANT, RADIO_MUX_ACT_DETACH, RADIO_MUX_AS_IDLE);
}

TEST_CASE("mode set while absent, probing or reopening: decided later", "[radio_mux]")
{
    const radio_mux_attach_state_t later[] = {
        RADIO_MUX_AS_ABSENT, RADIO_MUX_AS_PROBING, RADIO_MUX_AS_REOPENING, RADIO_MUX_AS_CLOSING,
    };

    for (size_t i = 0; i < sizeof(later) / sizeof(later[0]); i++) {
        radio_mux_attach_t a = at(later[i]);

        step(&a, RADIO_MUX_AEV_MODE_SET, WANT, RADIO_MUX_ACT_NONE, later[i]);
    }
}

/* ---- unplug ---- */

TEST_CASE("unplug of an attached transport: posts GONE, detaches and closes", "[radio_mux]")
{
    radio_mux_attach_t a = at(RADIO_MUX_AS_ATTACHED);

    step(&a, RADIO_MUX_AEV_UNPLUG, NO_WANT, RADIO_MUX_ACT_GONE_CLOSE, RADIO_MUX_AS_CLOSING);
}

TEST_CASE("unplug of an idle transport: closes without GONE", "[radio_mux]")
{
    radio_mux_attach_t a = at(RADIO_MUX_AS_IDLE);

    step(&a, RADIO_MUX_AEV_UNPLUG, NO_WANT, RADIO_MUX_ACT_CLOSE, RADIO_MUX_AS_CLOSING);
}

TEST_CASE("unplug while probing: closes, nothing attaches", "[radio_mux]")
{
    radio_mux_attach_t a = at(RADIO_MUX_AS_PROBING);

    step(&a, RADIO_MUX_AEV_UNPLUG, WANT, RADIO_MUX_ACT_CLOSE, RADIO_MUX_AS_CLOSING);
    // The detection result that was still on its way is stale.
    step(&a, RADIO_MUX_AEV_DETECT_DONE, WANT, RADIO_MUX_ACT_NONE, RADIO_MUX_AS_CLOSING);
    step(&a, RADIO_MUX_AEV_CLOSED, WANT, RADIO_MUX_ACT_NONE, RADIO_MUX_AS_ABSENT);
}

TEST_CASE("unplug while reopening: closes", "[radio_mux]")
{
    radio_mux_attach_t a = at(RADIO_MUX_AS_REOPENING);

    step(&a, RADIO_MUX_AEV_UNPLUG, WANT, RADIO_MUX_ACT_CLOSE, RADIO_MUX_AS_CLOSING);
    step(&a, RADIO_MUX_AEV_REOPENED, WANT, RADIO_MUX_ACT_NONE, RADIO_MUX_AS_CLOSING);
}

TEST_CASE("closed while closing: absent", "[radio_mux]")
{
    radio_mux_attach_t a = at(RADIO_MUX_AS_CLOSING);

    step(&a, RADIO_MUX_AEV_CLOSED, NO_WANT, RADIO_MUX_ACT_NONE, RADIO_MUX_AS_ABSENT);
}

TEST_CASE("unplug while absent: changes nothing", "[radio_mux]")
{
    radio_mux_attach_t a = at(RADIO_MUX_AS_ABSENT);

    step(&a, RADIO_MUX_AEV_UNPLUG, NO_WANT, RADIO_MUX_ACT_NONE, RADIO_MUX_AS_ABSENT);
}

/* ---- Review Focus 2: quick replug while the old handle is closing ---- */

TEST_CASE("plug while closing: waits for CLOSED", "[radio_mux]")
{
    radio_mux_attach_t a = at(RADIO_MUX_AS_CLOSING);

    step(&a, RADIO_MUX_AEV_PLUG, NO_WANT, RADIO_MUX_ACT_DEFER, RADIO_MUX_AS_CLOSING);
    TEST_ASSERT_TRUE(a.plug_pending);
}

TEST_CASE("closed after a deferred plug: probes the new device", "[radio_mux]")
{
    radio_mux_attach_t a = {.state = RADIO_MUX_AS_CLOSING, .plug_pending = true};

    step(&a, RADIO_MUX_AEV_CLOSED, NO_WANT, RADIO_MUX_ACT_PROBE, RADIO_MUX_AS_PROBING);
    TEST_ASSERT_FALSE(a.plug_pending);
}

TEST_CASE("unplug while closing with a deferred plug: the plug is forgotten", "[radio_mux]")
{
    radio_mux_attach_t a = {.state = RADIO_MUX_AS_CLOSING, .plug_pending = true};

    step(&a, RADIO_MUX_AEV_UNPLUG, NO_WANT, RADIO_MUX_ACT_NONE, RADIO_MUX_AS_CLOSING);
    TEST_ASSERT_FALSE(a.plug_pending);
    step(&a, RADIO_MUX_AEV_CLOSED, NO_WANT, RADIO_MUX_ACT_NONE, RADIO_MUX_AS_ABSENT);
}

TEST_CASE("quick replug of an attached device: one close, then detection and attach again", "[radio_mux]")
{
    const struct {
        radio_mux_attach_ev_t ev;
        radio_mux_attach_act_t act;
        radio_mux_attach_state_t next;
    } seq[] = {
        {RADIO_MUX_AEV_UNPLUG, RADIO_MUX_ACT_GONE_CLOSE, RADIO_MUX_AS_CLOSING},
        // The new device shows up before the old handle is closed.
        {RADIO_MUX_AEV_PLUG, RADIO_MUX_ACT_DEFER, RADIO_MUX_AS_CLOSING},
        {RADIO_MUX_AEV_CLOSED, RADIO_MUX_ACT_PROBE, RADIO_MUX_AS_PROBING},
        {RADIO_MUX_AEV_DETECT_DONE, RADIO_MUX_ACT_REOPEN, RADIO_MUX_AS_REOPENING},
        {RADIO_MUX_AEV_REOPENED, RADIO_MUX_ACT_ATTACH, RADIO_MUX_AS_ATTACHED},
    };
    radio_mux_attach_t a = at(RADIO_MUX_AS_ATTACHED);
    int closes = 0;

    for (size_t i = 0; i < sizeof(seq) / sizeof(seq[0]); i++) {
        const radio_mux_attach_act_t act = radio_mux_attach_next(&a, seq[i].ev, WANT);

        TEST_ASSERT_EQUAL(seq[i].act, act);
        TEST_ASSERT_EQUAL(seq[i].next, a.state);
        closes += act == RADIO_MUX_ACT_GONE_CLOSE || act == RADIO_MUX_ACT_CLOSE;
    }
    TEST_ASSERT_EQUAL(1, closes);
    TEST_ASSERT_FALSE(a.plug_pending);
}

TEST_CASE("replug of the same device after the close: GONE, present, attached again", "[radio_mux]")
{
    radio_mux_attach_t a = at(RADIO_MUX_AS_ATTACHED);

    step(&a, RADIO_MUX_AEV_UNPLUG, WANT, RADIO_MUX_ACT_GONE_CLOSE, RADIO_MUX_AS_CLOSING);
    step(&a, RADIO_MUX_AEV_CLOSED, WANT, RADIO_MUX_ACT_NONE, RADIO_MUX_AS_ABSENT);
    step(&a, RADIO_MUX_AEV_PLUG, WANT, RADIO_MUX_ACT_PROBE, RADIO_MUX_AS_PROBING);
    step(&a, RADIO_MUX_AEV_DETECT_DONE, WANT, RADIO_MUX_ACT_REOPEN, RADIO_MUX_AS_REOPENING);
    step(&a, RADIO_MUX_AEV_REOPENED, WANT, RADIO_MUX_ACT_ATTACH, RADIO_MUX_AS_ATTACHED);
}

/* ---- stale events ---- */

TEST_CASE("events that do not apply to the state: change nothing", "[radio_mux]")
{
    const struct {
        radio_mux_attach_state_t s;
        radio_mux_attach_ev_t ev;
    } stale[] = {
        {RADIO_MUX_AS_ABSENT, RADIO_MUX_AEV_DETECT_DONE},  {RADIO_MUX_AS_ABSENT, RADIO_MUX_AEV_REOPENED},
        {RADIO_MUX_AS_ABSENT, RADIO_MUX_AEV_CLOSED},       {RADIO_MUX_AS_PROBING, RADIO_MUX_AEV_REOPENED},
        {RADIO_MUX_AS_PROBING, RADIO_MUX_AEV_CLOSED},      {RADIO_MUX_AS_REOPENING, RADIO_MUX_AEV_DETECT_DONE},
        {RADIO_MUX_AS_IDLE, RADIO_MUX_AEV_DETECT_DONE},    {RADIO_MUX_AS_IDLE, RADIO_MUX_AEV_REOPENED},
        {RADIO_MUX_AS_IDLE, RADIO_MUX_AEV_CLOSED},         {RADIO_MUX_AS_ATTACHED, RADIO_MUX_AEV_DETECT_DONE},
        {RADIO_MUX_AS_ATTACHED, RADIO_MUX_AEV_REOPENED},   {RADIO_MUX_AS_ATTACHED, RADIO_MUX_AEV_CLOSED},
        {RADIO_MUX_AS_CLOSING, RADIO_MUX_AEV_ATTACH_FAILED},
    };

    for (size_t i = 0; i < sizeof(stale) / sizeof(stale[0]); i++) {
        radio_mux_attach_t a = at(stale[i].s);

        step(&a, stale[i].ev, WANT, RADIO_MUX_ACT_NONE, stale[i].s);
        TEST_ASSERT_FALSE(a.plug_pending);
    }
}

/* ---- the attach decision (R13) ---- */

static radio_mux_rules_t ziggurat_registered(void)
{
    radio_mux_rules_t r = {.n = 1};

    r.rules[0] = (radio_mux_rule_t) {
        .mode = RADIO_MUX_MODE_ZIGGURAT, .needs = RADIO_MUX_FW_SPINEL, .limit = 1, .reboot_on_change = true,
    };
    return r;
}

static radio_mux_chan_view_t view(radio_mux_mode_t stored, radio_mux_fw_t fw)
{
    return (radio_mux_chan_view_t) {
        .id = "usb-10c4-ea60", .mode = stored, .present = true, .supported = true, .fw = fw
    };
}

TEST_CASE("stored mode with a registered consumer and fitting firmware: attach wanted", "[radio_mux]")
{
    const radio_mux_rules_t r = ziggurat_registered();
    const radio_mux_chan_view_t otbr = view(RADIO_MUX_MODE_OTBR, RADIO_MUX_FW_SPINEL);
    const radio_mux_chan_view_t zig = view(RADIO_MUX_MODE_ZIGGURAT, RADIO_MUX_FW_SPINEL);

    TEST_ASSERT_TRUE(radio_mux_attach_wanted(&r, &otbr, true));
    TEST_ASSERT_TRUE(radio_mux_attach_wanted(&r, &zig, true));
}

TEST_CASE("the C6 with a registered consumer: attach wanted like any channel", "[radio_mux]")
{
    radio_mux_chan_view_t c6 = view(RADIO_MUX_MODE_ZIGGURAT, RADIO_MUX_FW_SPINEL);
    const radio_mux_rules_t r = ziggurat_registered();

    c6.id = "c6";
    TEST_ASSERT_TRUE(radio_mux_attach_wanted(&r, &c6, true));
}

TEST_CASE("firmware that does not fit the stored mode: no attach", "[radio_mux]")
{
    const radio_mux_rules_t r = ziggurat_registered();
    const radio_mux_chan_view_t otbr_znp = view(RADIO_MUX_MODE_OTBR, RADIO_MUX_FW_ZNP);
    const radio_mux_chan_view_t zig_on_zig = view(RADIO_MUX_MODE_ZIGGURAT, RADIO_MUX_FW_ZIGGURAT);
    const radio_mux_chan_view_t otbr_unknown = view(RADIO_MUX_MODE_OTBR, RADIO_MUX_FW_UNKNOWN);

    TEST_ASSERT_FALSE(radio_mux_attach_wanted(&r, &otbr_znp, true));
    TEST_ASSERT_FALSE(radio_mux_attach_wanted(&r, &zig_on_zig, true));
    TEST_ASSERT_FALSE(radio_mux_attach_wanted(&r, &otbr_unknown, true));
}

TEST_CASE("rcp-proxy with a registered consumer: attaches whatever the firmware", "[radio_mux]")
{
    const radio_mux_chan_view_t unknown = view(RADIO_MUX_MODE_RCP_PROXY, RADIO_MUX_FW_UNKNOWN);
    const radio_mux_chan_view_t ezsp = view(RADIO_MUX_MODE_RCP_PROXY, RADIO_MUX_FW_EZSP);

    TEST_ASSERT_TRUE(radio_mux_attach_wanted(NULL, &unknown, true));
    TEST_ASSERT_TRUE(radio_mux_attach_wanted(NULL, &ezsp, true));
}

TEST_CASE("no consumer registered for the stored mode: no attach", "[radio_mux]")
{
    const radio_mux_chan_view_t otbr = view(RADIO_MUX_MODE_OTBR, RADIO_MUX_FW_SPINEL);
    const radio_mux_chan_view_t proxy = view(RADIO_MUX_MODE_RCP_PROXY, RADIO_MUX_FW_SPINEL);

    TEST_ASSERT_FALSE(radio_mux_attach_wanted(NULL, &otbr, false));
    TEST_ASSERT_FALSE(radio_mux_attach_wanted(NULL, &proxy, false));
}

TEST_CASE("stored off or unavailable: no attach", "[radio_mux]")
{
    const radio_mux_chan_view_t off = view(RADIO_MUX_MODE_OFF, RADIO_MUX_FW_SPINEL);
    const radio_mux_chan_view_t unavail = view(RADIO_MUX_MODE_UNAVAILABLE, RADIO_MUX_FW_SPINEL);
    const radio_mux_chan_view_t invalid = view(RADIO_MUX_MODE_INVALID, RADIO_MUX_FW_SPINEL);

    TEST_ASSERT_FALSE(radio_mux_attach_wanted(NULL, &off, true));
    TEST_ASSERT_FALSE(radio_mux_attach_wanted(NULL, &unavail, true));
    TEST_ASSERT_FALSE(radio_mux_attach_wanted(NULL, &invalid, true));
}

TEST_CASE("channel not present or unsupported: no attach", "[radio_mux]")
{
    radio_mux_chan_view_t gone = view(RADIO_MUX_MODE_RCP_PROXY, RADIO_MUX_FW_SPINEL);
    radio_mux_chan_view_t unsupported = gone;

    gone.present = false;
    unsupported.supported = false;
    TEST_ASSERT_FALSE(radio_mux_attach_wanted(NULL, &gone, true));
    TEST_ASSERT_FALSE(radio_mux_attach_wanted(NULL, &unsupported, true));
}

/* ---- the RX post (Review Focus 5) ---- */

TEST_CASE("RX post into a queue with room: counts the frame and its bytes", "[radio_mux]")
{
    radio_mux_counters_t c = {0};

    TEST_ASSERT_EQUAL(RADIO_MUX_POSTED, radio_mux_post_rx(false, 42, &c));
    TEST_ASSERT_EQUAL_UINT32(1, c.frames_in);
    TEST_ASSERT_EQUAL_UINT64(42, c.bytes_in);
    TEST_ASSERT_EQUAL_UINT32(0, c.drops);
}

TEST_CASE("RX post into a full queue: dropped at once and counted", "[radio_mux]")
{
    radio_mux_counters_t c = {0};

    for (int i = 0; i < 3; i++) {
        TEST_ASSERT_EQUAL(RADIO_MUX_DROPPED, radio_mux_post_rx(true, 100, &c));
    }
    TEST_ASSERT_EQUAL_UINT32(3, c.drops);
    TEST_ASSERT_EQUAL_UINT32(0, c.frames_in);
    TEST_ASSERT_EQUAL_UINT64(0, c.bytes_in);
}

TEST_CASE("RX posts after drops: counting resumes, drops kept", "[radio_mux]")
{
    radio_mux_counters_t c = {0};

    radio_mux_post_rx(true, 10, &c);
    TEST_ASSERT_EQUAL(RADIO_MUX_POSTED, radio_mux_post_rx(false, 10, &c));
    TEST_ASSERT_EQUAL(RADIO_MUX_POSTED, radio_mux_post_rx(false, 5, &c));
    TEST_ASSERT_EQUAL_UINT32(2, c.frames_in);
    TEST_ASSERT_EQUAL_UINT64(15, c.bytes_in);
    TEST_ASSERT_EQUAL_UINT32(1, c.drops);
}

/* ---- NVS key ---- */

TEST_CASE("NVS key of the C6: c6", "[radio_mux]")
{
    char key[16];

    radio_mux_nvs_key("c6", key);
    TEST_ASSERT_EQUAL_STRING("c6", key);
}

TEST_CASE("NVS key of a USB channel: u and the FNV-1a hash of the id", "[radio_mux]")
{
    char key[16];

    radio_mux_nvs_key("usb-10c4-ea60", key);
    TEST_ASSERT_EQUAL_STRING("u8b535b6d", key);
    radio_mux_nvs_key("usb-1a86-7523", key);
    TEST_ASSERT_EQUAL_STRING("u15ec67fc", key);
    radio_mux_nvs_key("usb-10c4-ea60-9c2b1f", key);
    TEST_ASSERT_EQUAL_STRING("u5a0be22b", key);
}

/* ---- names ---- */

TEST_CASE("state names: the API's wire names", "[radio_mux]")
{
    TEST_ASSERT_EQUAL_STRING("idle", radio_mux_state_name(RADIO_MUX_STATE_IDLE));
    TEST_ASSERT_EQUAL_STRING("starting", radio_mux_state_name(RADIO_MUX_STATE_STARTING));
    TEST_ASSERT_EQUAL_STRING("running", radio_mux_state_name(RADIO_MUX_STATE_RUNNING));
    TEST_ASSERT_EQUAL_STRING("fault", radio_mux_state_name(RADIO_MUX_STATE_FAULT));
    TEST_ASSERT_EQUAL_STRING("busy", radio_mux_state_name(RADIO_MUX_STATE_BUSY));
    TEST_ASSERT_NULL(radio_mux_state_name((radio_mux_state_t)99));
}

TEST_CASE("firmware source names: probe and passive", "[radio_mux]")
{
    TEST_ASSERT_EQUAL_STRING("probe", radio_mux_fw_source_name(RADIO_MUX_FW_SRC_PROBE));
    TEST_ASSERT_EQUAL_STRING("passive", radio_mux_fw_source_name(RADIO_MUX_FW_SRC_PASSIVE));
    TEST_ASSERT_NULL(radio_mux_fw_source_name(RADIO_MUX_FW_SRC_NONE));
}

/* ---- stored rcp_proxy settings ---- */

TEST_CASE("rcp_proxy defaults: valid", "[radio_mux]")
{
    radio_mux_proxy_settings_t s;

    radio_mux_proxy_defaults(&s);
    TEST_ASSERT_TRUE(radio_mux_proxy_ok(&s));
}

TEST_CASE("rcp_proxy port 0, 80 or 179: invalid", "[radio_mux]")
{
    const uint16_t bad[] = {0, 80, 179};
    radio_mux_proxy_settings_t s;

    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        radio_mux_proxy_defaults(&s);
        s.port = bad[i];
        TEST_ASSERT_FALSE(radio_mux_proxy_ok(&s));
    }
    s.port = 65535;
    TEST_ASSERT_TRUE(radio_mux_proxy_ok(&s));
}

TEST_CASE("rcp_proxy connect: needs a host", "[radio_mux]")
{
    radio_mux_proxy_settings_t s;

    radio_mux_proxy_defaults(&s);
    s.dir = RADIO_MUX_PROXY_CONNECT;
    TEST_ASSERT_FALSE(radio_mux_proxy_ok(&s));
    strcpy(s.host, "192.168.1.10");
    TEST_ASSERT_TRUE(radio_mux_proxy_ok(&s));
}

TEST_CASE("rcp_proxy unknown direction, adapter or framing: invalid", "[radio_mux]")
{
    radio_mux_proxy_settings_t s;

    radio_mux_proxy_defaults(&s);
    s.dir = (radio_mux_proxy_dir_t)7;
    TEST_ASSERT_FALSE(radio_mux_proxy_ok(&s));
    radio_mux_proxy_defaults(&s);
    s.adapter = (radio_mux_proxy_adapter_t)7;
    TEST_ASSERT_FALSE(radio_mux_proxy_ok(&s));
    radio_mux_proxy_defaults(&s);
    s.framing = (radio_mux_proxy_framing_t)7;
    TEST_ASSERT_FALSE(radio_mux_proxy_ok(&s));
}

TEST_CASE("rcp_proxy host without its NUL: invalid", "[radio_mux]")
{
    radio_mux_proxy_settings_t s;

    radio_mux_proxy_defaults(&s);
    memset(s.host, 'a', sizeof(s.host));
    TEST_ASSERT_FALSE(radio_mux_proxy_ok(&s));
}

/* ---- TX timeout ---- */

TEST_CASE("TX timeout of a small write: the base", "[radio_mux]")
{
    // 1 byte at 460800: 2 * 10 bits / 460800 is well under a millisecond, rounded up to 1.
    TEST_ASSERT_EQUAL_UINT32(RADIO_MUX_TX_BASE_MS + 1, radio_mux_tx_timeout_ms(1, 460800));
}

TEST_CASE("TX timeout of a long frame at 115200: covers twice its wire time", "[radio_mux]")
{
    // 1300 bytes: 13000 bits, 112.85 ms on the wire; twice that, rounded up: 226 ms.
    const uint32_t t = radio_mux_tx_timeout_ms(1300, 115200);

    TEST_ASSERT_EQUAL_UINT32(RADIO_MUX_TX_BASE_MS + 226, t);
    TEST_ASSERT_GREATER_THAN_UINT32(113, t);
}

TEST_CASE("TX timeout of the largest HDLC frame at 38400: over two seconds", "[radio_mux]")
{
    // RADIO_MUX_HDLC_ENCODED_MAX(2048) = 4102 bytes: 41020 bits, 1068.2 ms on the wire; twice: 2137 ms.
    TEST_ASSERT_EQUAL_UINT32(RADIO_MUX_TX_BASE_MS + 2137, radio_mux_tx_timeout_ms(4102, 38400));
}

TEST_CASE("TX timeout with an unknown baud: counted at 38400", "[radio_mux]")
{
    TEST_ASSERT_EQUAL_UINT32(radio_mux_tx_timeout_ms(500, 38400), radio_mux_tx_timeout_ms(500, 0));
}

TEST_CASE("TX timeout of nothing: the base", "[radio_mux]")
{
    TEST_ASSERT_EQUAL_UINT32(RADIO_MUX_TX_BASE_MS, radio_mux_tx_timeout_ms(0, 115200));
}
