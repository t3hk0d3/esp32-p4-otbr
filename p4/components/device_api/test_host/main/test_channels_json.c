#include <stdio.h>
#include <string.h>
#include "unity.h"
#include "unity_test_runner.h"
#include "br_core_mode.h"
#include "channels_json.h"
#include "device_api_priv.h"
#include "rcp_sync.h"

static char body[DEVICE_API_CHANNELS_BODY_MAX];

// off, otbr and rcp-proxy: what a build without Zigbee registers.
#define MODES_BUILT_IN (RADIO_MUX_MODE_BIT(RADIO_MUX_MODE_OFF) | RADIO_MUX_MODE_BIT(RADIO_MUX_MODE_OTBR) | \
                        RADIO_MUX_MODE_BIT(RADIO_MUX_MODE_RCP_PROXY))

/* The C6 in otbr, running, with a probed firmware record. */
static radio_mux_chan_info_t c6_chan(void)
{
    radio_mux_chan_info_t c = {
        .kind = RADIO_MUX_KIND_C6,
        .present = true,
        .supported = true,
        .mode = RADIO_MUX_MODE_OTBR,
        .stored_mode = RADIO_MUX_MODE_OTBR,
        .state = RADIO_MUX_STATE_RUNNING,
        .line = {.baud = 460800, .flow_control = false},
        .caps = {.firmware_update = true, .hw_reset = true, .flow_control = false},
        .firmware = {.rec = {.type = RADIO_MUX_FW_SPINEL, .baud = 0}, .source = RADIO_MUX_FW_SRC_PASSIVE, .at = 1762430000},
        .counters = {.frames_in = 10, .frames_out = 9, .bytes_in = 1000, .bytes_out = 900, .drops = 1, .resets = 2, .rx_errors = 3},
    };

    strcpy(c.id, "c6");
    strcpy(c.label, "ESP32-C6 (on board)");
    strcpy(c.firmware.rec.version, "1.4.0");
    strcpy(c.firmware.rec.chip, "esp32c6");
    radio_mux_proxy_defaults(&c.proxy);
    return c;
}

/* A plugged-in dongle in rcp-proxy with a connected peer (as device_api fills it from rcp_proxy). */
static radio_mux_chan_info_t usb_chan(void)
{
    radio_mux_chan_info_t c = {
        .kind = RADIO_MUX_KIND_USB,
        .present = true,
        .supported = true,
        .mode = RADIO_MUX_MODE_RCP_PROXY,
        .stored_mode = RADIO_MUX_MODE_RCP_PROXY,
        .state = RADIO_MUX_STATE_RUNNING,
        .line = {.baud = 115200, .flow_control = true},
        .usb = {.vid = 0x10c4, .pid = 0xea60, .driver = RADIO_MUX_DRV_CP210X},
        .caps = {.firmware_update = false, .hw_reset = false, .flow_control = true},
        .proxy_state = "connected",
        .firmware = {.rec = {.type = RADIO_MUX_FW_ZNP, .baud = 115200}, .source = RADIO_MUX_FW_SRC_PROBE, .at = 0},
    };

    strcpy(c.id, "usb-10c4-ea60-abc123");
    strcpy(c.label, "Sonoff Zigbee 3.0 USB Dongle Plus");
    strcpy(c.usb.serial, "abc123");
    strcpy(c.firmware.rec.version, "20240710");
    radio_mux_proxy_defaults(&c.proxy);
    c.proxy.dir = RADIO_MUX_PROXY_CONNECT;
    c.proxy.adapter = RADIO_MUX_PROXY_UDP;
    c.proxy.port = 9999;
    strcpy(c.proxy.host, "host.local");
    c.proxy.framing = RADIO_MUX_PROXY_RAW;
    strcpy(c.proxy_peer, "192.168.1.20:9999");
    return c;
}

static int render_one(const radio_mux_chan_info_t *c)
{
    static radio_mux_snapshot_t s;

    memset(&s, 0, sizeof(s));
    s.available_modes = MODES_BUILT_IN;
    s.n = 1;
    s.chans[0] = *c;
    return device_api_channels_json(body, sizeof(body), &s);
}

/* ---- device_api_channels_json ---- */

TEST_CASE("On the registry snapshot: the C6 has the spec's fields in order, usb null", "[device_api]")
{
    const radio_mux_chan_info_t c = c6_chan();

    TEST_ASSERT_GREATER_THAN(0, render_one(&c));
    TEST_ASSERT_EQUAL_STRING(
        "{\"available_modes\":[\"off\",\"otbr\",\"rcp-proxy\"],\"channels\":[{\"id\":\"c6\",\"kind\":\"c6\",\"label\":\"ESP32-C6 (on board)\",\"mode\":\"otbr\","
        "\"stored_mode\":\"otbr\",\"state\":\"running\",\"error\":null,"
        "\"line\":{\"baud\":460800,\"flow_control\":false},\"usb\":null,"
        "\"caps\":{\"firmware_update\":true,\"hw_reset\":true,\"flow_control\":false},"
        "\"rcp_proxy\":{\"direction\":\"listen\",\"adapter\":\"tcp\",\"port\":6638,\"host\":\"\",\"framing\":\"hdlc\","
        "\"state\":null,\"peer\":null,\"last_error\":null},"
        "\"firmware\":{\"type\":\"spinel\",\"version\":\"1.4.0\",\"chip\":\"esp32c6\",\"baud\":null,\"source\":\"passive\","
        "\"at\":1762430000},"
        "\"counters\":{\"frames_in\":10,\"frames_out\":9,\"bytes_in\":1000,\"bytes_out\":900,\"drops\":1,\"resets\":2,"
        "\"rx_errors\":3}}]}",
        body);
}

TEST_CASE("On the registry snapshot: a dongle in rcp-proxy has usb, the proxy's state and peer, firmware", "[device_api]")
{
    const radio_mux_chan_info_t c = usb_chan();

    TEST_ASSERT_GREATER_THAN(0, render_one(&c));
    TEST_ASSERT_EQUAL_STRING(
        "{\"available_modes\":[\"off\",\"otbr\",\"rcp-proxy\"],\"channels\":[{\"id\":\"usb-10c4-ea60-abc123\",\"kind\":\"usb\",\"label\":\"Sonoff Zigbee 3.0 USB Dongle Plus\","
        "\"mode\":\"rcp-proxy\",\"stored_mode\":\"rcp-proxy\",\"state\":\"running\",\"error\":null,"
        "\"line\":{\"baud\":115200,\"flow_control\":true},"
        "\"usb\":{\"vid\":\"10c4\",\"pid\":\"ea60\",\"serial\":\"abc123\",\"driver\":\"cp210x\"},"
        "\"caps\":{\"firmware_update\":false,\"hw_reset\":false,\"flow_control\":true},"
        "\"rcp_proxy\":{\"direction\":\"connect\",\"adapter\":\"udp\",\"port\":9999,\"host\":\"host.local\",\"framing\":\"raw\","
        "\"state\":\"connected\",\"peer\":\"192.168.1.20:9999\",\"last_error\":null},"
        "\"firmware\":{\"type\":\"znp\",\"version\":\"20240710\",\"chip\":null,\"baud\":115200,\"source\":\"probe\",\"at\":null},"
        "\"counters\":{\"frames_in\":0,\"frames_out\":0,\"bytes_in\":0,\"bytes_out\":0,\"drops\":0,\"resets\":0,"
        "\"rx_errors\":0}}]}",
        body);
}

TEST_CASE("On the registry snapshot: channels in snapshot order, comma-separated", "[device_api]")
{
    static radio_mux_snapshot_t s;

    memset(&s, 0, sizeof(s));
    s.n = 2;
    s.chans[0] = c6_chan();
    s.chans[1] = usb_chan();
    TEST_ASSERT_GREATER_THAN(0, device_api_channels_json(body, sizeof(body), &s));
    const char *first = strstr(body, "{\"id\":\"c6\"");
    const char *second = strstr(body, ",{\"id\":\"usb-10c4-ea60-abc123\"");

    TEST_ASSERT_NOT_NULL(first);
    TEST_ASSERT_NOT_NULL(second);
    TEST_ASSERT_TRUE(first < second);
}

TEST_CASE("On no channel (before the mux starts): an empty list", "[device_api]")
{
    static radio_mux_snapshot_t s;

    memset(&s, 0, sizeof(s));
    TEST_ASSERT_GREATER_THAN(0, device_api_channels_json(body, sizeof(body), &s));
    TEST_ASSERT_EQUAL_STRING("{\"available_modes\":[],\"channels\":[]}", body);
}

TEST_CASE("On a build with every consumer: available_modes lists off, otbr, ziggurat, rcp-proxy in enum order",
          "[device_api]")
{
    static radio_mux_snapshot_t s;

    memset(&s, 0, sizeof(s));
    s.available_modes = MODES_BUILT_IN | RADIO_MUX_MODE_BIT(RADIO_MUX_MODE_ZIGGURAT);
    TEST_ASSERT_GREATER_THAN(0, device_api_channels_json(body, sizeof(body), &s));
    TEST_ASSERT_EQUAL_STRING("{\"available_modes\":[\"off\",\"otbr\",\"ziggurat\",\"rcp-proxy\"],\"channels\":[]}", body);
}

TEST_CASE("On only off set: available_modes renders [\"off\"]", "[device_api]")
{
    static radio_mux_snapshot_t s;

    memset(&s, 0, sizeof(s));
    s.available_modes = RADIO_MUX_MODE_BIT(RADIO_MUX_MODE_OFF);
    TEST_ASSERT_GREATER_THAN(0, device_api_channels_json(body, sizeof(body), &s));
    TEST_ASSERT_EQUAL_STRING("{\"available_modes\":[\"off\"],\"channels\":[]}", body);
}

TEST_CASE("On unavailable and invalid bits set: they are never listed", "[device_api]")
{
    static radio_mux_snapshot_t s;

    memset(&s, 0, sizeof(s));
    s.available_modes = RADIO_MUX_MODE_BIT(RADIO_MUX_MODE_OFF) | RADIO_MUX_MODE_BIT(RADIO_MUX_MODE_UNAVAILABLE) |
                        RADIO_MUX_MODE_BIT(RADIO_MUX_MODE_INVALID);
    TEST_ASSERT_GREATER_THAN(0, device_api_channels_json(body, sizeof(body), &s));
    TEST_ASSERT_EQUAL_STRING("{\"available_modes\":[\"off\"],\"channels\":[]}", body);
}

TEST_CASE("On an unplugged dongle: mode unavailable, stored_mode the mode it returns to", "[device_api]")
{
    radio_mux_chan_info_t c = usb_chan();

    c.present = false;
    c.mode = RADIO_MUX_MODE_UNAVAILABLE;
    c.stored_mode = RADIO_MUX_MODE_OTBR;
    c.state = RADIO_MUX_STATE_IDLE;
    c.proxy_state = NULL;
    c.proxy_peer[0] = '\0';
    strcpy(c.reason, "not detected");
    TEST_ASSERT_GREATER_THAN(0, render_one(&c));
    TEST_ASSERT_NOT_NULL(strstr(body, "\"mode\":\"unavailable\",\"stored_mode\":\"otbr\",\"state\":\"idle\","
                                "\"error\":\"not detected\""));
}

TEST_CASE("On a channel not in rcp-proxy: rcp_proxy holds the stored settings, state and peer null", "[device_api]")
{
    radio_mux_chan_info_t c = usb_chan();

    c.mode = RADIO_MUX_MODE_OFF;
    c.stored_mode = RADIO_MUX_MODE_OFF;
    c.state = RADIO_MUX_STATE_IDLE;
    c.proxy_state = NULL;
    c.proxy_peer[0] = '\0';
    TEST_ASSERT_GREATER_THAN(0, render_one(&c));
    TEST_ASSERT_NOT_NULL(strstr(body, "\"rcp_proxy\":{\"direction\":\"connect\",\"adapter\":\"udp\",\"port\":9999,"
                                "\"host\":\"host.local\",\"framing\":\"raw\",\"state\":null,\"peer\":null,"
                                "\"last_error\":null}"));
}

TEST_CASE("On a proxy without a peer: state written, peer null", "[device_api]")
{
    radio_mux_chan_info_t c = usb_chan();

    c.proxy_state = "listening";
    c.proxy_peer[0] = '\0';
    TEST_ASSERT_GREATER_THAN(0, render_one(&c));
    TEST_ASSERT_NOT_NULL(strstr(body, "\"state\":\"listening\",\"peer\":null,\"last_error\":null}"));
}

TEST_CASE("On a proxy in backoff after a failed open: last_error says why and is the channel's error", "[device_api]")
{
    radio_mux_chan_info_t c = usb_chan();

    c.proxy_state = "backoff";
    c.proxy_peer[0] = '\0';
    strcpy(c.proxy_last_error, "tcp port 6638: Address already in use");
    TEST_ASSERT_GREATER_THAN(0, render_one(&c));
    TEST_ASSERT_NOT_NULL(strstr(body, "\"state\":\"running\",\"error\":\"tcp port 6638: Address already in use\","));
    TEST_ASSERT_NOT_NULL(strstr(body, "\"state\":\"backoff\",\"peer\":null,"
                                "\"last_error\":\"tcp port 6638: Address already in use\"}"));
}

TEST_CASE("On a proxy that is not in backoff: last_error is shown, the channel's error stays its own", "[device_api]")
{
    radio_mux_chan_info_t c = usb_chan();

    c.proxy_state = "connecting";
    strcpy(c.proxy_last_error, "host.local does not resolve");
    TEST_ASSERT_GREATER_THAN(0, render_one(&c));
    TEST_ASSERT_NOT_NULL(strstr(body, "\"state\":\"running\",\"error\":null,"));
    TEST_ASSERT_NOT_NULL(strstr(body, "\"last_error\":\"host.local does not resolve\"}"));
}

TEST_CASE("On a proxy in backoff while the channel has a reason of its own: the reason stays the error", "[device_api]")
{
    radio_mux_chan_info_t c = usb_chan();

    c.proxy_state = "backoff";
    strcpy(c.proxy_last_error, "connect timed out");
    strcpy(c.reason, "flow control not supported");
    TEST_ASSERT_GREATER_THAN(0, render_one(&c));
    TEST_ASSERT_NOT_NULL(strstr(body, "\"error\":\"flow control not supported\","));
}

TEST_CASE("On a USB device without a driver: rcp_proxy and firmware null, usb driver null", "[device_api]")
{
    radio_mux_chan_info_t c = usb_chan();

    c.supported = false;
    c.mode = RADIO_MUX_MODE_UNAVAILABLE;
    c.stored_mode = RADIO_MUX_MODE_OFF;
    c.state = RADIO_MUX_STATE_IDLE;
    c.usb.driver = RADIO_MUX_DRV_NONE;
    c.usb.serial[0] = '\0';
    c.proxy_state = NULL;
    c.proxy_peer[0] = '\0';
    c.firmware.source = RADIO_MUX_FW_SRC_NONE;
    TEST_ASSERT_GREATER_THAN(0, render_one(&c));
    TEST_ASSERT_NOT_NULL(strstr(body, "\"usb\":{\"vid\":\"10c4\",\"pid\":\"ea60\",\"serial\":null,\"driver\":null}"));
    TEST_ASSERT_NOT_NULL(strstr(body, "\"rcp_proxy\":null,\"firmware\":null,\"counters\":"));
}

TEST_CASE("On a channel without a firmware record: firmware null", "[device_api]")
{
    radio_mux_chan_info_t c = c6_chan();

    c.firmware.source = RADIO_MUX_FW_SRC_NONE;
    TEST_ASSERT_GREATER_THAN(0, render_one(&c));
    TEST_ASSERT_NOT_NULL(strstr(body, "\"firmware\":null,\"counters\":"));
}

TEST_CASE("On a firmware record with an empty version and chip (ZNP without SYS_VERSION): both null", "[device_api]")
{
    radio_mux_chan_info_t c = usb_chan();

    c.firmware.rec.version[0] = '\0';
    c.firmware.rec.chip[0] = '\0';
    TEST_ASSERT_GREATER_THAN(0, render_one(&c));
    TEST_ASSERT_NOT_NULL(strstr(body, "\"firmware\":{\"type\":\"znp\",\"version\":null,\"chip\":null,"));
}

TEST_CASE("On a move that ended in a drain timeout: the target shows state fault, error drain_timeout", "[device_api]")
{
    radio_mux_chan_info_t c = usb_chan();

    c.mode = RADIO_MUX_MODE_OFF;
    c.stored_mode = RADIO_MUX_MODE_OFF;
    c.state = RADIO_MUX_STATE_FAULT;
    strcpy(c.reason, "drain_timeout");
    TEST_ASSERT_GREATER_THAN(0, render_one(&c));
    TEST_ASSERT_NOT_NULL(strstr(body, "\"state\":\"fault\",\"error\":\"drain_timeout\""));
}

TEST_CASE("On a channel being moved to (leased): state starting", "[device_api]")
{
    radio_mux_chan_info_t c = usb_chan();

    c.mode = RADIO_MUX_MODE_OTBR;
    c.stored_mode = RADIO_MUX_MODE_OFF;
    c.state = RADIO_MUX_STATE_STARTING;
    TEST_ASSERT_GREATER_THAN(0, render_one(&c));
    TEST_ASSERT_NOT_NULL(strstr(body, "\"mode\":\"otbr\",\"stored_mode\":\"off\",\"state\":\"starting\""));
}

TEST_CASE("On a label with quotes: it is escaped", "[device_api]")
{
    radio_mux_chan_info_t c = usb_chan();

    strcpy(c.label, "a \"b\"");
    TEST_ASSERT_GREATER_THAN(0, render_one(&c));
    TEST_ASSERT_NOT_NULL(strstr(body, "\"label\":\"a \\\"b\\\"\""));
}

TEST_CASE("On the largest snapshot (8 channels, every text at its longest): fits DEVICE_API_CHANNELS_BODY_MAX",
          "[device_api]")
{
    static radio_mux_snapshot_t s;

    memset(&s, 0, sizeof(s));
    s.available_modes = MODES_BUILT_IN | RADIO_MUX_MODE_BIT(RADIO_MUX_MODE_ZIGGURAT);
    s.n = RADIO_MUX_CHANNELS_MAX;
    for (size_t i = 0; i < RADIO_MUX_CHANNELS_MAX; i++) {
        radio_mux_chan_info_t c = usb_chan();

        memset(c.id, 'i', sizeof(c.id) - 1);
        c.id[sizeof(c.id) - 1] = '\0';
        memset(c.label, '"', sizeof(c.label) - 1); // escaped: twice as long on the wire
        c.label[sizeof(c.label) - 1] = '\0';
        memset(c.reason, '"', sizeof(c.reason) - 1);
        c.reason[sizeof(c.reason) - 1] = '\0';
        memset(c.usb.serial, 's', sizeof(c.usb.serial) - 1);
        c.usb.serial[sizeof(c.usb.serial) - 1] = '\0';
        memset(c.proxy.host, 'h', sizeof(c.proxy.host) - 1);
        c.proxy.host[sizeof(c.proxy.host) - 1] = '\0';
        memset(c.proxy_peer, 'p', sizeof(c.proxy_peer) - 1);
        c.proxy_peer[sizeof(c.proxy_peer) - 1] = '\0';
        memset(c.proxy_last_error, '"', sizeof(c.proxy_last_error) - 1);
        c.proxy_last_error[sizeof(c.proxy_last_error) - 1] = '\0';
        // Device text is printable ASCII (detect.c filters it): '"' and '\\' are the worst, 2 bytes each escaped.
        for (size_t k = 0; k + 1 < sizeof(c.firmware.rec.version); k++) {
            c.firmware.rec.version[k] = k % 2 ? '\\' : '"';
        }
        c.firmware.rec.version[sizeof(c.firmware.rec.version) - 1] = '\0';
        for (size_t k = 0; k + 1 < sizeof(c.firmware.rec.chip); k++) {
            c.firmware.rec.chip[k] = k % 2 ? '"' : '\\';
        }
        c.firmware.rec.chip[sizeof(c.firmware.rec.chip) - 1] = '\0';
        c.firmware.at = INT64_MAX;
        c.counters = (radio_mux_counters_t) {
            UINT32_MAX, UINT32_MAX, UINT64_MAX, UINT64_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX,
        };
        c.line.baud = 2000000;
        s.chans[i] = c;
    }
    TEST_ASSERT_GREATER_THAN(0, device_api_channels_json(body, sizeof(body), &s));
}

TEST_CASE("On a buffer too small for the body: returns -1", "[device_api]")
{
    static radio_mux_snapshot_t s;
    char small[64];

    memset(&s, 0, sizeof(s));
    s.n = 1;
    s.chans[0] = c6_chan();
    TEST_ASSERT_EQUAL(-1, device_api_channels_json(small, sizeof(small), &s));
}

/* ---- device_api_channel_json, device_api_channel_moving_json (PUT replies) ---- */

TEST_CASE("On an applied PUT: the reply is the channel object", "[device_api]")
{
    const radio_mux_chan_info_t c = c6_chan();
    char one[DEVICE_API_CHANNEL_BODY_MAX];

    TEST_ASSERT_GREATER_THAN(0, device_api_channel_json(one, sizeof(one), &c, false));
    TEST_ASSERT_EQUAL(0, strncmp(one, "{\"id\":\"c6\",\"kind\":\"c6\",", strlen("{\"id\":\"c6\",\"kind\":\"c6\",")));
    TEST_ASSERT_NULL(strstr(one, "reboot"));
    TEST_ASSERT_EQUAL('}', one[strlen(one) - 1]);
}

TEST_CASE("On a PUT that reboots: the channel object ends with reboot true", "[device_api]")
{
    const radio_mux_chan_info_t c = c6_chan();
    char one[DEVICE_API_CHANNEL_BODY_MAX];

    TEST_ASSERT_GREATER_THAN(0, device_api_channel_json(one, sizeof(one), &c, true));
    const size_t n = strlen(one);

    TEST_ASSERT_TRUE(n > 16);
    TEST_ASSERT_EQUAL_STRING("},\"reboot\":true}", one + n - 16);
}

TEST_CASE("On a move: the 202 body says moving and where from", "[device_api]")
{
    char out[96];

    TEST_ASSERT_GREATER_THAN(0, device_api_channel_moving_json(out, sizeof(out), "c6"));
    TEST_ASSERT_EQUAL_STRING("{\"moving\":true,\"moved_from\":\"c6\"}", out);
    TEST_ASSERT_GREATER_THAN(0, device_api_channel_moving_json(out, sizeof(out), ""));
    TEST_ASSERT_EQUAL_STRING("{\"moving\":true,\"moved_from\":null}", out);
}

/* ---- device_api_channel_put_parse ---- */

static esp_err_t parse(const char *json, radio_mux_set_req_t *req, const char **bad)
{
    memset(req, 0xa5, sizeof(*req)); // the parser must fill every field
    *bad = "untouched";
    return device_api_channel_put_parse(json, req, bad);
}

TEST_CASE("On an empty object: nothing is set", "[device_api]")
{
    radio_mux_set_req_t req;
    const char *bad;

    TEST_ASSERT_EQUAL(ESP_OK, parse("{}", &req, &bad));
    TEST_ASSERT_NULL(bad);
    TEST_ASSERT_FALSE(req.has_mode);
    TEST_ASSERT_FALSE(req.has_baud);
    TEST_ASSERT_FALSE(req.has_flow);
    TEST_ASSERT_FALSE(req.has_proxy);
    TEST_ASSERT_EQUAL(0, req.proxy_fields);
    TEST_ASSERT_FALSE(req.kick);
}

TEST_CASE("On each settable mode: has_mode and the mode", "[device_api]")
{
    static const struct {
        const char *json;
        radio_mux_mode_t mode;
    } cases[] = {
        {"{\"mode\":\"off\"}", RADIO_MUX_MODE_OFF},
        {"{\"mode\":\"otbr\"}", RADIO_MUX_MODE_OTBR},
        {"{\"mode\":\"ziggurat\"}", RADIO_MUX_MODE_ZIGGURAT},
        {"{\"mode\":\"rcp-proxy\"}", RADIO_MUX_MODE_RCP_PROXY},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        radio_mux_set_req_t req;
        const char *bad;

        TEST_ASSERT_EQUAL(ESP_OK, parse(cases[i].json, &req, &bad));
        TEST_ASSERT_TRUE(req.has_mode);
        TEST_ASSERT_EQUAL(cases[i].mode, req.mode);
        TEST_ASSERT_FALSE(req.has_baud);
    }
}

TEST_CASE("On a mode that is unavailable, unknown or not a string: 400 mode", "[device_api]")
{
    static const char *const bodies[] = {
        "{\"mode\":\"unavailable\"}", "{\"mode\":\"thread\"}", "{\"mode\":2}", "{\"mode\":null}", "{\"mode\":\"\"}",
    };

    for (size_t i = 0; i < sizeof(bodies) / sizeof(bodies[0]); i++) {
        radio_mux_set_req_t req;
        const char *bad;

        TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, parse(bodies[i], &req, &bad));
        TEST_ASSERT_EQUAL_STRING("mode", bad);
    }
}

TEST_CASE("On an accepted baud: has_baud and the baud", "[device_api]")
{
    radio_mux_set_req_t req;
    const char *bad;

    TEST_ASSERT_EQUAL(ESP_OK, parse("{\"baud\":921600}", &req, &bad));
    TEST_ASSERT_TRUE(req.has_baud);
    TEST_ASSERT_EQUAL_UINT32(921600, req.baud);
    TEST_ASSERT_FALSE(req.has_mode);
}

TEST_CASE("On a baud outside the list, fractional, negative or not a number: 400 baud", "[device_api]")
{
    static const char *const bodies[] = {
        "{\"baud\":12345}", "{\"baud\":115200.5}", "{\"baud\":-115200}", "{\"baud\":\"115200\"}", "{\"baud\":1e20}",
    };

    for (size_t i = 0; i < sizeof(bodies) / sizeof(bodies[0]); i++) {
        radio_mux_set_req_t req;
        const char *bad;

        TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, parse(bodies[i], &req, &bad));
        TEST_ASSERT_EQUAL_STRING("baud", bad);
    }
}

TEST_CASE("On flow_control true or false: has_flow and the value; not a boolean: 400 flow_control", "[device_api]")
{
    radio_mux_set_req_t req;
    const char *bad;

    TEST_ASSERT_EQUAL(ESP_OK, parse("{\"flow_control\":true}", &req, &bad));
    TEST_ASSERT_TRUE(req.has_flow);
    TEST_ASSERT_TRUE(req.flow);
    TEST_ASSERT_EQUAL(ESP_OK, parse("{\"flow_control\":false}", &req, &bad));
    TEST_ASSERT_TRUE(req.has_flow);
    TEST_ASSERT_FALSE(req.flow);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, parse("{\"flow_control\":1}", &req, &bad));
    TEST_ASSERT_EQUAL_STRING("flow_control", bad);
}

TEST_CASE("On every rcp_proxy setting: has_proxy, the values and their field bits", "[device_api]")
{
    radio_mux_set_req_t req;
    const char *bad;

    TEST_ASSERT_EQUAL(ESP_OK, parse("{\"rcp_proxy\":{\"direction\":\"connect\",\"adapter\":\"udp\",\"port\":9999,"
                                    "\"host\":\"fd00::1\",\"framing\":\"raw\"}}", &req, &bad));
    TEST_ASSERT_TRUE(req.has_proxy);
    TEST_ASSERT_EQUAL(RADIO_MUX_PROXY_F_DIR | RADIO_MUX_PROXY_F_ADAPTER | RADIO_MUX_PROXY_F_PORT | RADIO_MUX_PROXY_F_HOST |
                      RADIO_MUX_PROXY_F_FRAMING, req.proxy_fields);
    TEST_ASSERT_EQUAL(RADIO_MUX_PROXY_CONNECT, req.proxy.dir);
    TEST_ASSERT_EQUAL(RADIO_MUX_PROXY_UDP, req.proxy.adapter);
    TEST_ASSERT_EQUAL_UINT16(9999, req.proxy.port);
    TEST_ASSERT_EQUAL_STRING("fd00::1", req.proxy.host);
    TEST_ASSERT_EQUAL(RADIO_MUX_PROXY_RAW, req.proxy.framing);
    TEST_ASSERT_FALSE(req.kick);
}

TEST_CASE("On some rcp_proxy settings: only their bits; the other values are the defaults", "[device_api]")
{
    radio_mux_set_req_t req;
    const char *bad;

    TEST_ASSERT_EQUAL(ESP_OK, parse("{\"rcp_proxy\":{\"direction\":\"listen\",\"adapter\":\"tcp\",\"framing\":\"hdlc\"}}",
                                    &req, &bad));
    TEST_ASSERT_EQUAL(RADIO_MUX_PROXY_F_DIR | RADIO_MUX_PROXY_F_ADAPTER | RADIO_MUX_PROXY_F_FRAMING, req.proxy_fields);
    TEST_ASSERT_EQUAL(RADIO_MUX_PROXY_LISTEN, req.proxy.dir);
    TEST_ASSERT_EQUAL(RADIO_MUX_PROXY_TCP, req.proxy.adapter);
    TEST_ASSERT_EQUAL(RADIO_MUX_PROXY_HDLC, req.proxy.framing);
    TEST_ASSERT_EQUAL_UINT16(RADIO_MUX_PROXY_DEFAULT_PORT, req.proxy.port);
    TEST_ASSERT_EQUAL_STRING("", req.proxy.host);
    TEST_ASSERT_EQUAL(ESP_OK, parse("{\"rcp_proxy\":{\"host\":\"\"}}", &req, &bad));
    TEST_ASSERT_EQUAL(RADIO_MUX_PROXY_F_HOST, req.proxy_fields);
}

TEST_CASE("On rcp_proxy kick true: kick set, no setting given; kick false: nothing", "[device_api]")
{
    radio_mux_set_req_t req;
    const char *bad;

    TEST_ASSERT_EQUAL(ESP_OK, parse("{\"rcp_proxy\":{\"kick\":true}}", &req, &bad));
    TEST_ASSERT_TRUE(req.kick);
    TEST_ASSERT_FALSE(req.has_proxy);
    TEST_ASSERT_EQUAL(0, req.proxy_fields);
    TEST_ASSERT_EQUAL(ESP_OK, parse("{\"rcp_proxy\":{\"kick\":false}}", &req, &bad));
    TEST_ASSERT_FALSE(req.kick);
    TEST_ASSERT_FALSE(req.has_proxy);
    TEST_ASSERT_EQUAL(ESP_OK, parse("{\"rcp_proxy\":{\"kick\":true,\"port\":7000}}", &req, &bad));
    TEST_ASSERT_TRUE(req.kick);
    TEST_ASSERT_TRUE(req.has_proxy);
    TEST_ASSERT_EQUAL(RADIO_MUX_PROXY_F_PORT, req.proxy_fields);
}

TEST_CASE("On a wrong rcp_proxy value or type: 400 with the nested field", "[device_api]")
{
    static const struct {
        const char *json;
        const char *field;
    } cases[] = {
        {"{\"rcp_proxy\":[]}", "rcp_proxy"},
        {"{\"rcp_proxy\":\"tcp\"}", "rcp_proxy"},
        {"{\"rcp_proxy\":{\"direction\":\"both\"}}", "rcp_proxy.direction"},
        {"{\"rcp_proxy\":{\"direction\":1}}", "rcp_proxy.direction"},
        {"{\"rcp_proxy\":{\"adapter\":\"sctp\"}}", "rcp_proxy.adapter"},
        {"{\"rcp_proxy\":{\"port\":0}}", "rcp_proxy.port"},
        {"{\"rcp_proxy\":{\"port\":65536}}", "rcp_proxy.port"},
        {"{\"rcp_proxy\":{\"port\":6638.5}}", "rcp_proxy.port"},
        {"{\"rcp_proxy\":{\"port\":\"6638\"}}", "rcp_proxy.port"},
        {"{\"rcp_proxy\":{\"host\":5}}", "rcp_proxy.host"},
        {
            "{\"rcp_proxy\":{\"host\":\"0123456789012345678901234567890123456789012345678901234567890123\"}}",
            "rcp_proxy.host"
        },
        {"{\"rcp_proxy\":{\"framing\":\"slip\"}}", "rcp_proxy.framing"},
        {"{\"rcp_proxy\":{\"kick\":\"yes\"}}", "rcp_proxy.kick"},
        {"{\"rcp_proxy\":{\"peer\":\"x\"}}", "rcp_proxy.peer"},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        radio_mux_set_req_t req;
        const char *bad;

        TEST_ASSERT_EQUAL_MESSAGE(ESP_ERR_INVALID_ARG, parse(cases[i].json, &req, &bad), cases[i].json);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(cases[i].field, bad, cases[i].json);
    }
}

TEST_CASE("On a body that is not a JSON object: 400 body", "[device_api]")
{
    static const char *const bodies[] = {"", "{", "[]", "\"otbr\"", "null"};

    for (size_t i = 0; i < sizeof(bodies) / sizeof(bodies[0]); i++) {
        radio_mux_set_req_t req;
        const char *bad;

        TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, parse(bodies[i], &req, &bad));
        TEST_ASSERT_EQUAL_STRING("body", bad);
    }
}

TEST_CASE("On an unknown field (a GET channel sent back): 400 with its name", "[device_api]")
{
    radio_mux_set_req_t req;
    const char *bad;

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, parse("{\"mode\":\"off\",\"state\":\"idle\"}", &req, &bad));
    TEST_ASSERT_EQUAL_STRING("state", bad);
}

TEST_CASE("On all fields together: each is set", "[device_api]")
{
    radio_mux_set_req_t req;
    const char *bad;

    TEST_ASSERT_EQUAL(ESP_OK, parse("{\"mode\":\"rcp-proxy\",\"baud\":460800,\"flow_control\":false,"
                                    "\"rcp_proxy\":{\"port\":7000}}", &req, &bad));
    TEST_ASSERT_TRUE(req.has_mode && req.has_baud && req.has_flow && req.has_proxy);
    TEST_ASSERT_EQUAL(RADIO_MUX_MODE_RCP_PROXY, req.mode);
    TEST_ASSERT_EQUAL_UINT32(460800, req.baud);
    TEST_ASSERT_FALSE(req.flow);
    TEST_ASSERT_EQUAL_UINT16(7000, req.proxy.port);
}

/* ---- device_api_channel_proxy_refusal: rcp_proxy_settings_check on the merged settings ---- */

static radio_mux_set_req_t proxy_req(const char *json)
{
    radio_mux_set_req_t req;
    const char *bad;

    TEST_ASSERT_EQUAL(ESP_OK, device_api_channel_put_parse(json, &req, &bad));
    return req;
}

TEST_CASE("On connect without a host, stored or given: 400 rcp_proxy.host", "[device_api]")
{
    radio_mux_proxy_settings_t stored;
    const radio_mux_set_req_t req = proxy_req("{\"rcp_proxy\":{\"direction\":\"connect\"}}");

    radio_mux_proxy_defaults(&stored);
    TEST_ASSERT_EQUAL_STRING("rcp_proxy.host", device_api_channel_proxy_refusal(&stored, &req));
}

TEST_CASE("On connect with the host stored earlier: accepted", "[device_api]")
{
    radio_mux_proxy_settings_t stored;
    const radio_mux_set_req_t req = proxy_req("{\"rcp_proxy\":{\"direction\":\"connect\"}}");

    radio_mux_proxy_defaults(&stored);
    strcpy(stored.host, "192.168.1.20");
    TEST_ASSERT_NULL(device_api_channel_proxy_refusal(&stored, &req));
}

TEST_CASE("On a port of the board's own listeners (80, 179): 400 rcp_proxy.port", "[device_api]")
{
    radio_mux_proxy_settings_t stored;

    radio_mux_proxy_defaults(&stored);
    radio_mux_set_req_t req = proxy_req("{\"rcp_proxy\":{\"port\":80}}");

    TEST_ASSERT_EQUAL_STRING("rcp_proxy.port", device_api_channel_proxy_refusal(&stored, &req));
    req = proxy_req("{\"rcp_proxy\":{\"port\":179}}");
    TEST_ASSERT_EQUAL_STRING("rcp_proxy.port", device_api_channel_proxy_refusal(&stored, &req));
}

TEST_CASE("On a host that is no address or name: 400 rcp_proxy.host", "[device_api]")
{
    radio_mux_proxy_settings_t stored;
    const radio_mux_set_req_t req = proxy_req("{\"rcp_proxy\":{\"host\":\"bad host!\"}}");

    radio_mux_proxy_defaults(&stored);
    TEST_ASSERT_EQUAL_STRING("rcp_proxy.host", device_api_channel_proxy_refusal(&stored, &req));
}

TEST_CASE("On a request without rcp_proxy settings: nothing to check", "[device_api]")
{
    radio_mux_proxy_settings_t stored;
    const radio_mux_set_req_t req = proxy_req("{\"rcp_proxy\":{\"kick\":true}}");

    radio_mux_proxy_defaults(&stored);
    stored.port = 80; // stored before the rule existed: not the request's fault
    TEST_ASSERT_NULL(device_api_channel_proxy_refusal(&stored, &req));
}

/* ---- device_api_channel_refusal ---- */

TEST_CASE("On each verdict: its status and code", "[device_api]")
{
    static const struct {
        radio_mux_verdict_t v;
        int status;
        const char *code;
    } cases[] = {
        {RADIO_MUX_APPLY, 200, NULL},
        {RADIO_MUX_APPLY_REBOOT, 200, NULL},
        {RADIO_MUX_MOVE, 202, NULL},
        {RADIO_MUX_REFUSE_BAD_VALUE, 400, "bad_value"},
        {RADIO_MUX_REFUSE_UNAVAILABLE, 409, "unavailable"},
        {RADIO_MUX_REFUSE_NOT_AVAILABLE, 409, "not_available"},
        {RADIO_MUX_REFUSE_INCOMPATIBLE, 409, "incompatible"},
        {RADIO_MUX_REFUSE_MODE_TAKEN, 409, "mode_taken"},
        {RADIO_MUX_REFUSE_HYBRID, 409, "hybrid_not_supported"},
        {RADIO_MUX_REFUSE_UNKNOWN_CHANNEL, 404, "unknown_channel"},
        {RADIO_MUX_REFUSE_FAILED, 500, "failed"},
        {RADIO_MUX_REFUSE_PROBE_FAILED, 409, "probe_failed"},
        {RADIO_MUX_REFUSE_BUSY, 409, "busy"},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        int status = 0;
        const char *code = "untouched";

        device_api_channel_refusal(cases[i].v, &status, &code);
        TEST_ASSERT_EQUAL_MESSAGE(cases[i].status, status, cases[i].code ? cases[i].code : "accepted");
        if (cases[i].code) {
            TEST_ASSERT_EQUAL_STRING(cases[i].code, code);
            TEST_ASSERT_NOT_NULL(device_api_channel_refusal_detail(cases[i].v));
            TEST_ASSERT_TRUE(device_api_channel_refusal_detail(cases[i].v)[0] != '\0');
        } else {
            TEST_ASSERT_NULL(code);
        }
    }
}

TEST_CASE("On a verdict out of range: 500 failed", "[device_api]")
{
    int status = 0;
    const char *code = NULL;

    device_api_channel_refusal((radio_mux_verdict_t)99, &status, &code);
    TEST_ASSERT_EQUAL(500, status);
    TEST_ASSERT_EQUAL_STRING("failed", code);
}

TEST_CASE("On a C6 push or recovery while rcp-proxy on the C6 has a peer: 409 c6_busy", "[device_api]")
{
    device_api_error_t e = device_api_map_push_error(RCP_SYNC_ERR_C6_BUSY);

    TEST_ASSERT_EQUAL(409, e.http_status);
    TEST_ASSERT_EQUAL_STRING("c6_busy", e.code);
    e = device_api_map_recover_error(RCP_SYNC_ERR_C6_BUSY);
    TEST_ASSERT_EQUAL(409, e.http_status);
    TEST_ASSERT_EQUAL_STRING("c6_busy", e.code);

    char out[160];
    int status = 0;

    TEST_ASSERT_GREATER_THAN(0, device_api_recover_reply(RCP_SYNC_ERR_C6_BUSY, "c6_busy: rcp-proxy on the C6 has a peer",
                                                         BR_MODE_BR_RUNNING, "", true, "1.0", out, sizeof(out), &status));
    TEST_ASSERT_EQUAL(409, status);
    TEST_ASSERT_NOT_NULL(strstr(out, "\"error\":\"c6_busy\""));
}

/* ---- device_api_channel_forget_error (DELETE) ---- */

TEST_CASE("On a forget: 204, 404 unknown channel, 409 for the C6 or a present channel, 500 otherwise", "[device_api]")
{
    TEST_ASSERT_EQUAL(204, device_api_channel_forget_error(ESP_OK, true).http_status);
    device_api_error_t e = device_api_channel_forget_error(ESP_ERR_NOT_FOUND, true);

    TEST_ASSERT_EQUAL(404, e.http_status);
    TEST_ASSERT_EQUAL_STRING("unknown_channel", e.code);
    e = device_api_channel_forget_error(ESP_ERR_INVALID_STATE, true);
    TEST_ASSERT_EQUAL(409, e.http_status);
    TEST_ASSERT_EQUAL_STRING("in_use", e.code);
    TEST_ASSERT_NOT_NULL(e.detail);
    e = device_api_channel_forget_error(ESP_FAIL, true);
    TEST_ASSERT_EQUAL(500, e.http_status);
    TEST_ASSERT_EQUAL_STRING("failed", e.code);
    TEST_ASSERT_EQUAL_STRING("204 No Content", device_api_status_line(204));
}

TEST_CASE("On a forget while the radio mux is not running: 503 not_started, not in_use", "[device_api]")
{
    const device_api_error_t e = device_api_channel_forget_error(ESP_ERR_INVALID_STATE, false);

    TEST_ASSERT_EQUAL(503, e.http_status);
    TEST_ASSERT_EQUAL_STRING("not_started", e.code);
    TEST_ASSERT_NOT_NULL(strstr(e.detail, "not running"));
}

/* ---- device_api_channel_move_refusal, device_api_channel_move_busy ---- */

TEST_CASE("On a move of Thread that also carries line or proxy settings: 400, nothing to claim", "[device_api]")
{
    static const char *const bodies[] = {
        "{\"mode\":\"otbr\",\"baud\":460800}",
        "{\"mode\":\"otbr\",\"flow_control\":false}",
        "{\"mode\":\"otbr\",\"rcp_proxy\":{\"port\":7000}}",
        "{\"mode\":\"otbr\",\"rcp_proxy\":{\"kick\":true}}",
    };

    for (size_t i = 0; i < sizeof(bodies) / sizeof(bodies[0]); i++) {
        const radio_mux_set_req_t req = proxy_req(bodies[i]);

        TEST_ASSERT_EQUAL_STRING_MESSAGE("send line or proxy settings in a separate request from moving Thread",
                                         device_api_channel_move_refusal(&req, "usb-1", "c6"), bodies[i]);
    }
}

TEST_CASE("On a move of Thread alone, or settings with otbr that is no move: accepted", "[device_api]")
{
    radio_mux_set_req_t req = proxy_req("{\"mode\":\"otbr\"}");

    TEST_ASSERT_NULL(device_api_channel_move_refusal(&req, "usb-1", "c6"));
    req = proxy_req("{\"mode\":\"otbr\",\"baud\":460800}");
    TEST_ASSERT_NULL(device_api_channel_move_refusal(&req, "c6", "c6"));  // already there: no move
    TEST_ASSERT_NULL(device_api_channel_move_refusal(&req, "usb-1", NULL)); // nothing has it: no move
    req = proxy_req("{\"mode\":\"rcp-proxy\",\"rcp_proxy\":{\"port\":7000}}");
    TEST_ASSERT_NULL(device_api_channel_move_refusal(&req, "usb-1", "c6"));
}

TEST_CASE("On a mode PUT while a move of Thread runs: 409 busy on any channel", "[device_api]")
{
    radio_mux_set_req_t req = proxy_req("{\"mode\":\"off\"}");

    TEST_ASSERT_EQUAL_STRING("OpenThread is being moved to another radio", device_api_channel_move_busy(&req, true));
    TEST_ASSERT_NULL(device_api_channel_move_busy(&req, false));
    req = proxy_req("{\"baud\":115200}"); // not a mode change
    TEST_ASSERT_NULL(device_api_channel_move_busy(&req, true));
}

/* ---- device_api_channel_port_conflict ---- */

/* Two channels: the C6 stored in rcp-proxy listening on tcp/6638, a dongle stored off with the same stored settings. */
static void port_snapshot(radio_mux_snapshot_t *s)
{
    memset(s, 0, sizeof(*s));
    s->n = 2;
    s->chans[0] = c6_chan();
    s->chans[0].mode = s->chans[0].stored_mode = RADIO_MUX_MODE_RCP_PROXY;
    s->chans[1] = usb_chan();
    s->chans[1].mode = s->chans[1].stored_mode = RADIO_MUX_MODE_OFF;
    radio_mux_proxy_defaults(&s->chans[1].proxy);
}

TEST_CASE("On rcp-proxy for a channel that would listen where another channel in rcp-proxy listens: names it",
          "[device_api]")
{
    static radio_mux_snapshot_t s;
    radio_mux_set_req_t req = proxy_req("{\"mode\":\"rcp-proxy\"}");

    port_snapshot(&s);
    TEST_ASSERT_EQUAL_STRING("c6", device_api_channel_port_conflict(&s, s.chans[1].id, &req));
}

TEST_CASE("On a port change of a channel in rcp-proxy to a port another one listens on: names it", "[device_api]")
{
    static radio_mux_snapshot_t s;
    radio_mux_set_req_t req = proxy_req("{\"rcp_proxy\":{\"port\":6638}}");

    port_snapshot(&s);
    s.chans[1].stored_mode = RADIO_MUX_MODE_RCP_PROXY;
    s.chans[1].proxy.port = 7000;
    TEST_ASSERT_EQUAL_STRING("c6", device_api_channel_port_conflict(&s, s.chans[1].id, &req));
}

TEST_CASE("On another port, adapter or direction: no conflict", "[device_api]")
{
    static radio_mux_snapshot_t s;
    radio_mux_set_req_t req = proxy_req("{\"mode\":\"rcp-proxy\",\"rcp_proxy\":{\"port\":6639}}");

    port_snapshot(&s);
    TEST_ASSERT_NULL(device_api_channel_port_conflict(&s, s.chans[1].id, &req));
    req = proxy_req("{\"mode\":\"rcp-proxy\",\"rcp_proxy\":{\"adapter\":\"udp\"}}");
    TEST_ASSERT_NULL(device_api_channel_port_conflict(&s, s.chans[1].id, &req));
    req = proxy_req("{\"mode\":\"rcp-proxy\",\"rcp_proxy\":{\"direction\":\"connect\",\"host\":\"h.local\"}}");
    TEST_ASSERT_NULL(device_api_channel_port_conflict(&s, s.chans[1].id, &req));
    req = proxy_req("{\"mode\":\"rcp-proxy\"}");
    s.chans[0].proxy.dir = RADIO_MUX_PROXY_CONNECT; // the other one dials out
    TEST_ASSERT_NULL(device_api_channel_port_conflict(&s, s.chans[1].id, &req));
}

TEST_CASE("On another channel that only stores the port (not in rcp-proxy): no conflict", "[device_api]")
{
    static radio_mux_snapshot_t s;
    radio_mux_set_req_t req = proxy_req("{\"mode\":\"rcp-proxy\"}");

    port_snapshot(&s);
    s.chans[0].stored_mode = RADIO_MUX_MODE_OTBR;
    TEST_ASSERT_NULL(device_api_channel_port_conflict(&s, s.chans[1].id, &req));
}

TEST_CASE("On an unplugged channel stored in rcp-proxy on the port: a conflict (it listens again on return)",
          "[device_api]")
{
    static radio_mux_snapshot_t s;
    radio_mux_set_req_t req = proxy_req("{\"mode\":\"rcp-proxy\"}");

    port_snapshot(&s);
    s.chans[1].stored_mode = RADIO_MUX_MODE_RCP_PROXY;
    s.chans[1].mode = RADIO_MUX_MODE_UNAVAILABLE;
    s.chans[1].present = false;
    TEST_ASSERT_EQUAL_STRING(s.chans[1].id, device_api_channel_port_conflict(&s, "c6", &req));
}

TEST_CASE("On a PUT that leaves rcp-proxy, no change of mode or port, or the channel alone: no conflict",
          "[device_api]")
{
    static radio_mux_snapshot_t s;
    radio_mux_set_req_t req = proxy_req("{\"mode\":\"off\"}");

    port_snapshot(&s);
    s.chans[1].stored_mode = RADIO_MUX_MODE_RCP_PROXY;
    TEST_ASSERT_NULL(device_api_channel_port_conflict(&s, s.chans[1].id, &req));
    req = proxy_req("{\"baud\":115200}");
    s.chans[1].stored_mode = RADIO_MUX_MODE_OFF;
    TEST_ASSERT_NULL(device_api_channel_port_conflict(&s, s.chans[1].id, &req)); // stays off
    req = proxy_req("{\"mode\":\"rcp-proxy\"}");
    s.n = 1;
    TEST_ASSERT_NULL(device_api_channel_port_conflict(&s, "c6", &req)); // itself only
    TEST_ASSERT_NULL(device_api_channel_port_conflict(&s, "usb-unknown", &req)); // not in the list
}

/* ---- device_api_channel_is_move, device_api_channel_retry_otbr ---- */

TEST_CASE("On a PUT of otbr while another channel has it: it is a move (the long-operation slot)", "[device_api]")
{
    radio_mux_set_req_t req = proxy_req("{\"mode\":\"otbr\"}");

    TEST_ASSERT_TRUE(device_api_channel_is_move(&req, "usb-10c4-ea60", "c6"));
    TEST_ASSERT_FALSE(device_api_channel_is_move(&req, "c6", "c6"));    // already there
    TEST_ASSERT_FALSE(device_api_channel_is_move(&req, "c6", NULL));    // nothing to move from
    req = proxy_req("{\"mode\":\"off\"}");
    TEST_ASSERT_FALSE(device_api_channel_is_move(&req, "usb-10c4-ea60", "c6"));
    req = proxy_req("{\"baud\":115200}");
    TEST_ASSERT_FALSE(device_api_channel_is_move(&req, "usb-10c4-ea60", "c6"));
}

TEST_CASE("On a PUT of otbr on the OTBR channel in RADIO_FAULT: OpenThread's start is retried", "[device_api]")
{
    radio_mux_set_req_t req = proxy_req("{\"mode\":\"otbr\"}");

    TEST_ASSERT_TRUE(device_api_channel_retry_otbr(&req, RADIO_MUX_APPLY, "usb-1", "usb-1", BR_MODE_RADIO_FAULT));
    TEST_ASSERT_FALSE(device_api_channel_retry_otbr(&req, RADIO_MUX_APPLY, "usb-1", "usb-1", BR_MODE_BR_RUNNING));
    TEST_ASSERT_FALSE(device_api_channel_retry_otbr(&req, RADIO_MUX_REFUSE_BUSY, "usb-1", "usb-1", BR_MODE_RADIO_FAULT));
    TEST_ASSERT_FALSE(device_api_channel_retry_otbr(&req, RADIO_MUX_APPLY, "usb-2", "usb-1", BR_MODE_RADIO_FAULT));
    TEST_ASSERT_FALSE(device_api_channel_retry_otbr(&req, RADIO_MUX_APPLY, "usb-1", NULL, BR_MODE_RADIO_FAULT));
    req = proxy_req("{\"baud\":115200}");
    TEST_ASSERT_FALSE(device_api_channel_retry_otbr(&req, RADIO_MUX_APPLY, "usb-1", "usb-1", BR_MODE_RADIO_FAULT));
}

/* ---- device_api_channels_overlay_otbr ---- */

TEST_CASE("On RADIO_FAULT: the running OTBR channel shows fault with the board's reason", "[device_api]")
{
    static radio_mux_snapshot_t s;

    memset(&s, 0, sizeof(s));
    s.n = 2;
    s.chans[0] = c6_chan();
    s.chans[1] = usb_chan();
    device_api_channels_overlay_otbr(&s, "c6", BR_MODE_RADIO_FAULT, "rcp incompatible");
    TEST_ASSERT_EQUAL(RADIO_MUX_STATE_FAULT, s.chans[0].state);
    TEST_ASSERT_EQUAL_STRING("rcp incompatible", s.chans[0].reason);
    TEST_ASSERT_EQUAL(RADIO_MUX_STATE_RUNNING, s.chans[1].state); // not the OTBR channel
}

TEST_CASE("On any other mode or no OTBR channel: the channels are left as the mux says", "[device_api]")
{
    static radio_mux_snapshot_t s;

    memset(&s, 0, sizeof(s));
    s.n = 1;
    s.chans[0] = c6_chan();
    device_api_channels_overlay_otbr(&s, "c6", BR_MODE_BR_RUNNING, "");
    device_api_channels_overlay_otbr(&s, "c6", BR_MODE_STARTING, "");
    device_api_channels_overlay_otbr(&s, NULL, BR_MODE_RADIO_FAULT, "x");
    TEST_ASSERT_EQUAL(RADIO_MUX_STATE_RUNNING, s.chans[0].state);
    TEST_ASSERT_EQUAL_STRING("", s.chans[0].reason);
    s.chans[0].state = RADIO_MUX_STATE_BUSY; // a C6 job: its own state wins
    device_api_channels_overlay_otbr(&s, "c6", BR_MODE_RADIO_FAULT, "x");
    TEST_ASSERT_EQUAL(RADIO_MUX_STATE_BUSY, s.chans[0].state);
}
