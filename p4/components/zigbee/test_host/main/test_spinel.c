#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "unity.h"
#include "spinel.h"
#include "frames.h"
#include "zigbee_spinel.h"

/*
 * A fake RCP behind the ops seam. send() records the request and, like OpenThread's RCP (NcpBase), queues its
 * reply; wait() passes one queued frame to zigbee_spinel_on_frame per call, or moves the clock to the deadline and
 * returns nonzero when nothing is queued.
 */

#define MAX_SENT 160
#define MAX_QUEUE 32
#define START_US 1000000LL

typedef struct {
    uint8_t buf[ZIGBEE_SPINEL_FRAME_MAX];
    size_t len;
} frame_t;

static zigbee_spinel_t client;

static struct {
    int64_t now;
    frame_t sent[MAX_SENT];
    size_t sent_count;
    frame_t queue[MAX_QUEUE];
    size_t q_head, q_tail;
    frame_t inject[4]; // queued ahead of the reply to the next request
    size_t inject_count;

    unsigned major, minor, radio_caps;
    const unsigned *caps;
    size_t caps_count;
    const uint8_t *eui64;
    const char *ncp_version;
    bool reset_silent;
    uint8_t reset_iid;
    unsigned fail_key, fail_status;
    unsigned silent_key;
    unsigned wrong_key; // answered with VALUE_IS of another property
    size_t short_limit, ext_limit; // 0 = unlimited
    size_t short_count, ext_count;

    int64_t step_us; // clock advance per delivered frame
    int send_result;
    int resets_requested;
} rcp;

static struct {
    int ready_count;
    bool compatible;
    char reason[128];
    int rx_count;
    uint8_t psdu[ZIGBEE_SPINEL_PSDU_MAX];
    size_t psdu_len;
    uint8_t channel, lqi;
    int8_t rssi;
    uint64_t time_us;
    int tx_count;
    zigbee_tx_result_t tx;
    int scan_count;
    int8_t scan_rssi;
} got;

static const zigbee_radio_config_t CONFIG = {
    .channel = 15,
    .tx_power_dbm = 8,
    .pan_id = 0x1a62,
    .short_addr = 0x0000,
    .ext_addr = {0x00, 0x12, 0x4b, 0x00, 0x01, 0x02, 0x03, 0x04},
    .promiscuous = false,
};
static const uint8_t CONFIG_EXT_REVERSED[8] = {0x04, 0x03, 0x02, 0x01, 0x00, 0x4b, 0x12, 0x00};
static const uint8_t EXT_A[8] = {0x00, 0x15, 0x8d, 0x00, 0x00, 0x00, 0x00, 0x01};
static const uint8_t EXT_A_REVERSED[8] = {0x01, 0x00, 0x00, 0x00, 0x00, 0x8d, 0x15, 0x00};
static const uint8_t EXT_B[8] = {0x00, 0x15, 0x8d, 0x00, 0x00, 0x00, 0x00, 0x02};

static void queue_frame(const uint8_t *buf, size_t len)
{
    TEST_ASSERT_TRUE(rcp.q_tail - rcp.q_head < MAX_QUEUE);
    frame_t *f = &rcp.queue[rcp.q_tail++ % MAX_QUEUE];
    memcpy(f->buf, buf, len);
    f->len = len;
}

static void reply_last_status(uint8_t tid, unsigned status)
{
    uint8_t buf[32];
    queue_frame(buf, frames_last_status(buf, sizeof(buf), tid, status));
}

static void respond(const uint8_t *frame, size_t len)
{
    uint8_t header = 0;
    unsigned cmd = 0, key = 0;
    const uint8_t *data = NULL;
    unsigned dlen = 0;
    uint8_t buf[ZIGBEE_SPINEL_FRAME_MAX];

    for (size_t i = 0; i < rcp.inject_count; i++) {
        queue_frame(rcp.inject[i].buf, rcp.inject[i].len);
    }
    rcp.inject_count = 0;

    TEST_ASSERT_TRUE(spinel_datatype_unpack(frame, len, "CiiD", &header, &cmd, &key, &data, &dlen) > 0);
    uint8_t tid = header & 0x0f;

    if (cmd == SPINEL_CMD_RESET) {
        if (!rcp.reset_silent) {
            queue_frame(buf, frames_reset_status(buf, sizeof(buf), rcp.reset_iid, SPINEL_STATUS_RESET_POWER_ON));
        }
        return;
    }
    if (key == rcp.silent_key) {
        return;
    }
    if (key == rcp.wrong_key) {
        static const uint8_t value = 0;
        queue_frame(buf, frames_value_is(buf, sizeof(buf), tid, SPINEL_PROP_PHY_RSSI, &value, 1));
        return;
    }
    if (key == rcp.fail_key) {
        reply_last_status(tid, rcp.fail_status);
        return;
    }
    if (cmd == SPINEL_CMD_PROP_VALUE_GET) {
        switch (key) {
        case SPINEL_PROP_PROTOCOL_VERSION:
            queue_frame(buf, frames_protocol_version(buf, sizeof(buf), tid, rcp.major, rcp.minor));
            break;
        case SPINEL_PROP_NCP_VERSION:
            queue_frame(buf, frames_ncp_version(buf, sizeof(buf), tid, rcp.ncp_version));
            break;
        case SPINEL_PROP_CAPS:
            queue_frame(buf, frames_caps(buf, sizeof(buf), tid, rcp.caps, rcp.caps_count));
            break;
        case SPINEL_PROP_RADIO_CAPS:
            queue_frame(buf, frames_radio_caps(buf, sizeof(buf), tid, rcp.radio_caps));
            break;
        case SPINEL_PROP_HWADDR:
            queue_frame(buf, frames_hwaddr(buf, sizeof(buf), tid, rcp.eui64));
            break;
        default:
            reply_last_status(tid, SPINEL_STATUS_PROP_NOT_FOUND);
            break;
        }
    } else if (cmd == SPINEL_CMD_PROP_VALUE_SET) {
        if (key == SPINEL_PROP_STREAM_RAW) {
            return; // answered by the test, as the transmit's result
        }
        if (key == SPINEL_PROP_MAC_SRC_MATCH_SHORT_ADDRESSES || key == SPINEL_PROP_MAC_SRC_MATCH_EXTENDED_ADDRESSES) {
            // No getter: NcpBase answers a set with LAST_STATUS OK (WritePropertyValueIsFrame).
            if (key == SPINEL_PROP_MAC_SRC_MATCH_SHORT_ADDRESSES) {
                rcp.short_count = 0;
            } else {
                rcp.ext_count = 0;
            }
            reply_last_status(tid, SPINEL_STATUS_OK);
            return;
        }
        queue_frame(buf, frames_value_is(buf, sizeof(buf), tid, key, data, dlen));
    } else if (cmd == SPINEL_CMD_PROP_VALUE_INSERT) {
        bool is_short = key == SPINEL_PROP_MAC_SRC_MATCH_SHORT_ADDRESSES;
        size_t *count = is_short ? &rcp.short_count : &rcp.ext_count;
        size_t limit = is_short ? rcp.short_limit : rcp.ext_limit;

        if (limit != 0 && *count >= limit) {
            reply_last_status(tid, SPINEL_STATUS_NOMEM); // otLinkRawSrcMatchAdd*Entry: OT_ERROR_NO_BUFS
            return;
        }
        (*count)++;
        queue_frame(buf, frames_inserted(buf, sizeof(buf), tid, key, data, dlen));
    }
}

static int fake_send(void *ctx, const uint8_t *frame, size_t len)
{
    (void)ctx;
    TEST_ASSERT_TRUE(rcp.sent_count < MAX_SENT);
    TEST_ASSERT_TRUE(len <= ZIGBEE_SPINEL_FRAME_MAX);
    if (rcp.send_result != 0) {
        return rcp.send_result;
    }
    memcpy(rcp.sent[rcp.sent_count].buf, frame, len);
    rcp.sent[rcp.sent_count].len = len;
    rcp.sent_count++;
    respond(frame, len);
    return 0;
}

static void fake_request_reset(void *ctx)
{
    (void)ctx;
    rcp.resets_requested++;
}

static int64_t fake_now(void *ctx)
{
    (void)ctx;
    return rcp.now;
}

static int fake_wait(void *ctx, int64_t until_us)
{
    (void)ctx;
    if (rcp.q_head == rcp.q_tail) {
        if (until_us > rcp.now) {
            rcp.now = until_us;
        }
        return 1;
    }
    frame_t f = rcp.queue[rcp.q_head++ % MAX_QUEUE];
    rcp.now += rcp.step_us;
    zigbee_spinel_on_frame(&client, f.buf, f.len, rcp.now);
    return 0;
}

static void sink_ready(void *ctx, bool compatible, const char *reason)
{
    (void)ctx;
    got.ready_count++;
    got.compatible = compatible;
    snprintf(got.reason, sizeof(got.reason), "%s", reason ? reason : "");
}

static void sink_rx(void *ctx, const uint8_t *psdu, size_t len, uint8_t channel, int8_t rssi, uint8_t lqi,
                    uint64_t time_us)
{
    (void)ctx;
    got.rx_count++;
    TEST_ASSERT_TRUE(len <= sizeof(got.psdu));
    memcpy(got.psdu, psdu, len);
    got.psdu_len = len;
    got.channel = channel;
    got.rssi = rssi;
    got.lqi = lqi;
    got.time_us = time_us;
}

static void sink_tx_done(void *ctx, zigbee_tx_result_t r)
{
    (void)ctx;
    got.tx_count++;
    got.tx = r;
}

static void sink_scan_done(void *ctx, int8_t max_rssi)
{
    (void)ctx;
    got.scan_count++;
    got.scan_rssi = max_rssi;
}

static const zigbee_spinel_ops_t OPS = {
    .send = fake_send, .request_reset = fake_request_reset, .now_us = fake_now, .wait = fake_wait, .ctx = NULL,
};
static const zigbee_spinel_sink_t SINK = {
    .ready = sink_ready, .rx = sink_rx, .tx_done = sink_tx_done, .scan_done = sink_scan_done, .ctx = NULL,
};

static void setup(unsigned radio_caps)
{
    memset(&rcp, 0, sizeof(rcp));
    memset(&got, 0, sizeof(got));
    rcp.now = START_US;
    rcp.step_us = 100;
    rcp.major = 4;
    rcp.minor = 3;
    rcp.radio_caps = radio_caps;
    rcp.caps = FRAMES_RCP_CAPS;
    rcp.caps_count = sizeof(FRAMES_RCP_CAPS) / sizeof(FRAMES_RCP_CAPS[0]);
    rcp.eui64 = radio_caps == FRAMES_CC2652P_RADIO_CAPS ? FRAMES_CC2652P_EUI64 : FRAMES_C6_EUI64;
    rcp.ncp_version = radio_caps == FRAMES_CC2652P_RADIO_CAPS ? FRAMES_CC2652P_NCP_VERSION : FRAMES_C6_NCP_VERSION;
    rcp.silent_key = UINT32_MAX;
    rcp.fail_key = UINT32_MAX;
    rcp.wrong_key = UINT32_MAX;
    zigbee_spinel_init(&client, &OPS, &SINK);
}

/* Attached to an RCP with `radio_caps`; the attach's frames are forgotten. */
static void attached(unsigned radio_caps)
{
    setup(radio_caps);
    zigbee_spinel_attach(&client);
    TEST_ASSERT_EQUAL(1, got.ready_count);
    TEST_ASSERT_TRUE(got.compatible);
    rcp.sent_count = 0;
    got.ready_count = 0;
}

typedef struct {
    uint8_t header;
    unsigned cmd, key;
    const uint8_t *data;
    unsigned len;
} req_t;

static req_t sent(size_t i)
{
    req_t r = {0};

    TEST_ASSERT_TRUE_MESSAGE(i < rcp.sent_count, "no such request");
    TEST_ASSERT_TRUE(spinel_datatype_unpack(rcp.sent[i].buf, rcp.sent[i].len, "CiiD", &r.header, &r.cmd, &r.key,
                                            &r.data, &r.len) > 0);
    return r;
}

static uint8_t sent_tid(size_t i)
{
    return sent(i).header & 0x0f;
}

static void assert_sent(size_t i, unsigned cmd, unsigned key)
{
    req_t r = sent(i);
    char msg[64];

    snprintf(msg, sizeof(msg), "request %zu", i);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(cmd, r.cmd, msg);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(key, r.key, msg);
}

static bool sent_bool(size_t i)
{
    req_t r = sent(i);
    bool v = false;

    TEST_ASSERT_TRUE(spinel_datatype_unpack(r.data, r.len, "b", &v) > 0);
    return v;
}

static size_t count_sent(unsigned cmd, unsigned key)
{
    size_t n = 0;

    for (size_t i = 0; i < rcp.sent_count; i++) {
        req_t r = sent(i);
        n += r.cmd == cmd && r.key == key;
    }
    return n;
}

static void deliver(const uint8_t *buf, size_t len)
{
    zigbee_spinel_on_frame(&client, buf, len, rcp.now);
}

/* ---- attach ---- */

TEST_CASE("attach on a fresh channel: sends CMD_RESET first, then reads PROTOCOL_VERSION, NCP_VERSION, CAPS, "
          "RADIO_CAPS, HWADDR", "[zigbee]")
{
    static const uint8_t reset[] = {0x80, SPINEL_CMD_RESET, SPINEL_RESET_STACK}; // SpinelDriver::SendReset

    setup(FRAMES_C6_RADIO_CAPS);
    zigbee_spinel_attach(&client);
    TEST_ASSERT_EQUAL(sizeof(reset), rcp.sent[0].len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(reset, rcp.sent[0].buf, sizeof(reset));
    assert_sent(1, SPINEL_CMD_PROP_VALUE_GET, SPINEL_PROP_PROTOCOL_VERSION);
    assert_sent(2, SPINEL_CMD_PROP_VALUE_GET, SPINEL_PROP_NCP_VERSION);
    assert_sent(3, SPINEL_CMD_PROP_VALUE_GET, SPINEL_PROP_CAPS);
    assert_sent(4, SPINEL_CMD_PROP_VALUE_GET, SPINEL_PROP_RADIO_CAPS);
    assert_sent(5, SPINEL_CMD_PROP_VALUE_GET, SPINEL_PROP_HWADDR);
    for (size_t i = 1; i <= 5; i++) {
        TEST_ASSERT_EQUAL_HEX8(0x80, sent(i).header & 0xf0); // IID 0
        TEST_ASSERT_NOT_EQUAL(0, sent_tid(i));
        TEST_ASSERT_EQUAL(0, sent(i).len); // a get carries no value
    }
}

TEST_CASE("attach on a fresh channel: sets PHY_ENABLED and MAC_RAW_STREAM_ENABLED after the reads", "[zigbee]")
{
    setup(FRAMES_C6_RADIO_CAPS);
    zigbee_spinel_attach(&client);
    TEST_ASSERT_EQUAL(8, rcp.sent_count);
    assert_sent(6, SPINEL_CMD_PROP_VALUE_SET, SPINEL_PROP_PHY_ENABLED);
    TEST_ASSERT_TRUE(sent_bool(6));
    assert_sent(7, SPINEL_CMD_PROP_VALUE_SET, SPINEL_PROP_MAC_RAW_STREAM_ENABLED);
    TEST_ASSERT_TRUE(sent_bool(7));
}

TEST_CASE("attach on a fresh channel: reports ready as compatible", "[zigbee]")
{
    setup(FRAMES_C6_RADIO_CAPS);
    zigbee_spinel_attach(&client);
    TEST_ASSERT_EQUAL(1, got.ready_count);
    TEST_ASSERT_TRUE(got.compatible);
}

TEST_CASE("attach on a fresh channel: keeps the EUI-64 as HWADDR reported it", "[zigbee]")
{
    uint8_t eui[8] = {0};

    setup(FRAMES_C6_RADIO_CAPS);
    TEST_ASSERT_FALSE(zigbee_spinel_eui64(&client, eui));
    zigbee_spinel_attach(&client);
    TEST_ASSERT_TRUE(zigbee_spinel_eui64(&client, eui));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(FRAMES_C6_EUI64, eui, 8);
}

TEST_CASE("attach on the CC2652P (radio caps 0x7f): reports ready as compatible", "[zigbee]")
{
    setup(FRAMES_CC2652P_RADIO_CAPS);
    zigbee_spinel_attach(&client);
    TEST_ASSERT_EQUAL(1, got.ready_count);
    TEST_ASSERT_TRUE(got.compatible);
}

TEST_CASE("attach with protocol 4.2: the minor version is not checked", "[zigbee]")
{
    setup(FRAMES_C6_RADIO_CAPS);
    rcp.minor = 2;
    zigbee_spinel_attach(&client);
    TEST_ASSERT_TRUE(got.compatible);
}

TEST_CASE("attach with the reset status on the broadcast IID: the reset is accepted", "[zigbee]")
{
    setup(FRAMES_C6_RADIO_CAPS);
    rcp.reset_iid = 3;
    zigbee_spinel_attach(&client);
    TEST_ASSERT_EQUAL(1, got.ready_count);
    TEST_ASSERT_TRUE(got.compatible);
}

TEST_CASE("attach after a configuration and source-match edits: replays them after the raw stream is on", "[zigbee]")
{
    attached(FRAMES_C6_RADIO_CAPS);
    TEST_ASSERT_EQUAL(0, zigbee_spinel_configure(&client, &CONFIG));
    zigbee_spinel_src_match_clear(&client);
    TEST_ASSERT_EQUAL(0, zigbee_spinel_src_match_add_short(&client, 0x1234));
    TEST_ASSERT_EQUAL(0, zigbee_spinel_src_match_add_ext(&client, EXT_A));
    rcp.sent_count = 0;

    zigbee_spinel_attach(&client);
    assert_sent(7, SPINEL_CMD_PROP_VALUE_SET, SPINEL_PROP_MAC_RAW_STREAM_ENABLED);
    assert_sent(8, SPINEL_CMD_PROP_VALUE_SET, SPINEL_PROP_PHY_CHAN);
    assert_sent(15, SPINEL_CMD_PROP_VALUE_SET, SPINEL_PROP_MAC_RX_ON_WHEN_IDLE_MODE);
    assert_sent(16, SPINEL_CMD_PROP_VALUE_SET, SPINEL_PROP_MAC_SRC_MATCH_SHORT_ADDRESSES);
    assert_sent(17, SPINEL_CMD_PROP_VALUE_SET, SPINEL_PROP_MAC_SRC_MATCH_EXTENDED_ADDRESSES);
    assert_sent(18, SPINEL_CMD_PROP_VALUE_INSERT, SPINEL_PROP_MAC_SRC_MATCH_SHORT_ADDRESSES);
    assert_sent(19, SPINEL_CMD_PROP_VALUE_INSERT, SPINEL_PROP_MAC_SRC_MATCH_EXTENDED_ADDRESSES);
    TEST_ASSERT_EQUAL(20, rcp.sent_count);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(EXT_A_REVERSED, sent(19).data, 8);
    TEST_ASSERT_EQUAL(1, got.ready_count);
    TEST_ASSERT_TRUE(got.compatible);
}

TEST_CASE("attach with protocol major version 3: reports incompatible with the reason", "[zigbee]")
{
    setup(FRAMES_C6_RADIO_CAPS);
    rcp.major = 3;
    zigbee_spinel_attach(&client);
    TEST_ASSERT_EQUAL(1, got.ready_count);
    TEST_ASSERT_FALSE(got.compatible);
    TEST_ASSERT_NOT_NULL(strstr(got.reason, "protocol 3.3"));
}

TEST_CASE("attach with protocol major version 3: sends nothing more", "[zigbee]")
{
    setup(FRAMES_C6_RADIO_CAPS);
    rcp.major = 3;
    zigbee_spinel_attach(&client);
    TEST_ASSERT_EQUAL(2, rcp.sent_count);
    TEST_ASSERT_NOT_EQUAL(0, zigbee_spinel_transmit(&client, (const uint8_t *)"\x41\x88", 2, 15, true, 3, 4));
    TEST_ASSERT_EQUAL(2, rcp.sent_count);
}

TEST_CASE("attach with a missing required radio cap: reports incompatible naming it", "[zigbee]")
{
    setup(FRAMES_C6_RADIO_CAPS & ~FRAMES_CAPS_CSMA_BACKOFF & ~FRAMES_CAPS_ENERGY_SCAN);
    zigbee_spinel_attach(&client);
    TEST_ASSERT_EQUAL(1, got.ready_count);
    TEST_ASSERT_FALSE(got.compatible);
    TEST_ASSERT_NOT_NULL(strstr(got.reason, "energy-scan"));
    TEST_ASSERT_NOT_NULL(strstr(got.reason, "CSMA-backoff"));
}

TEST_CASE("attach with a missing required radio cap: sends nothing more", "[zigbee]")
{
    setup(FRAMES_C6_RADIO_CAPS & ~FRAMES_CAPS_TRANSMIT_RETRIES);
    zigbee_spinel_attach(&client);
    TEST_ASSERT_FALSE(got.compatible);
    TEST_ASSERT_EQUAL(5, rcp.sent_count); // reset and four reads; no HWADDR, PHY_ENABLED or raw stream
}

TEST_CASE("attach with no ACK-timeout radio cap: reports incompatible", "[zigbee]")
{
    setup(FRAMES_C6_RADIO_CAPS & ~FRAMES_CAPS_ACK_TIMEOUT);
    zigbee_spinel_attach(&client);
    TEST_ASSERT_FALSE(got.compatible);
    TEST_ASSERT_NOT_NULL(strstr(got.reason, "ack-timeout"));
}

TEST_CASE("attach to a coprocessor without the radio and raw-MAC caps: reports incompatible", "[zigbee]")
{
    static const unsigned ncp_caps[] = {SPINEL_CAP_COUNTERS, SPINEL_CAP_CONFIG_FTD};

    setup(FRAMES_C6_RADIO_CAPS);
    rcp.caps = ncp_caps;
    rcp.caps_count = 2;
    zigbee_spinel_attach(&client);
    TEST_ASSERT_EQUAL(1, got.ready_count);
    TEST_ASSERT_FALSE(got.compatible);
    TEST_ASSERT_NOT_NULL(strstr(got.reason, "not an RCP"));
    TEST_ASSERT_EQUAL(4, rcp.sent_count);
}

/* ---- request timeout ---- */

TEST_CASE("request timeout during attach: asks for a channel reset and reports nothing", "[zigbee]")
{
    setup(FRAMES_C6_RADIO_CAPS);
    rcp.reset_silent = true;
    zigbee_spinel_attach(&client);
    TEST_ASSERT_EQUAL(1, rcp.resets_requested);
    TEST_ASSERT_EQUAL(0, got.ready_count);
    TEST_ASSERT_EQUAL(1, rcp.sent_count);
    TEST_ASSERT_EQUAL_INT64(START_US + ZIGBEE_SPINEL_REQUEST_TIMEOUT_US, rcp.now);
}

TEST_CASE("request timeout of a property request: returns nonzero after 2000 ms", "[zigbee]")
{
    attached(FRAMES_C6_RADIO_CAPS);
    rcp.silent_key = SPINEL_PROP_PHY_CHAN;
    int64_t start = rcp.now;
    TEST_ASSERT_NOT_EQUAL(0, zigbee_spinel_set_channel(&client, 20));
    TEST_ASSERT_EQUAL_INT64(start + 2000 * 1000LL, rcp.now);
}

TEST_CASE("request timeout of a property request: asks for a channel reset", "[zigbee]")
{
    attached(FRAMES_C6_RADIO_CAPS);
    rcp.silent_key = SPINEL_PROP_PHY_CHAN;
    zigbee_spinel_set_channel(&client, 20);
    TEST_ASSERT_EQUAL(1, rcp.resets_requested);
}

TEST_CASE("request timeout with a chatty RCP: other frames do not hold the request past 2000 ms", "[zigbee]")
{
    uint8_t buf[64];
    static const uint8_t psdu[5] = {0x41, 0x88, 0x01, 0x00, 0x00};

    attached(FRAMES_C6_RADIO_CAPS);
    rcp.silent_key = SPINEL_PROP_PHY_CHAN;
    size_t n = frames_stream_raw_rx(buf, sizeof(buf), 0, psdu, sizeof(psdu), -50, 15, 200, 0, 0);
    for (int i = 0; i < MAX_QUEUE; i++) {
        queue_frame(buf, n);
    }
    rcp.step_us = 100 * 1000; // twenty frames take the whole 2000 ms
    TEST_ASSERT_NOT_EQUAL(0, zigbee_spinel_set_channel(&client, 20));
    TEST_ASSERT_EQUAL(1, rcp.resets_requested);
    TEST_ASSERT_TRUE(rcp.q_head < rcp.q_tail); // gave up without draining the queue
}

TEST_CASE("request timeout: the next request gets its reply", "[zigbee]")
{
    attached(FRAMES_C6_RADIO_CAPS);
    rcp.silent_key = SPINEL_PROP_PHY_CHAN;
    zigbee_spinel_set_channel(&client, 20);
    rcp.silent_key = UINT32_MAX;
    TEST_ASSERT_EQUAL(0, zigbee_spinel_set_channel(&client, 21));
}

TEST_CASE("request ids: property requests use TIDs 1..15 and never 0", "[zigbee]")
{
    attached(FRAMES_C6_RADIO_CAPS);
    for (int i = 0; i < 40; i++) {
        TEST_ASSERT_EQUAL(0, zigbee_spinel_set_channel(&client, (uint8_t)(11 + i % 16)));
    }
    for (size_t i = 0; i < rcp.sent_count; i++) {
        uint8_t tid = sent_tid(i);
        TEST_ASSERT_TRUE(tid >= 1 && tid <= 15);
    }
}

/* ---- configure ---- */

TEST_CASE("configure: sends PHY_CHAN, PHY_TX_POWER, MAC_15_4_PANID, MAC_15_4_SADDR, MAC_15_4_LADDR, "
          "MAC_PROMISCUOUS_MODE, MAC_SRC_MATCH_ENABLED, MAC_RX_ON_WHEN_IDLE_MODE", "[zigbee]")
{
    attached(FRAMES_C6_RADIO_CAPS);
    TEST_ASSERT_EQUAL(0, zigbee_spinel_configure(&client, &CONFIG));
    TEST_ASSERT_EQUAL(8, rcp.sent_count);
    assert_sent(0, SPINEL_CMD_PROP_VALUE_SET, SPINEL_PROP_PHY_CHAN);
    assert_sent(1, SPINEL_CMD_PROP_VALUE_SET, SPINEL_PROP_PHY_TX_POWER);
    assert_sent(2, SPINEL_CMD_PROP_VALUE_SET, SPINEL_PROP_MAC_15_4_PANID);
    assert_sent(3, SPINEL_CMD_PROP_VALUE_SET, SPINEL_PROP_MAC_15_4_SADDR);
    assert_sent(4, SPINEL_CMD_PROP_VALUE_SET, SPINEL_PROP_MAC_15_4_LADDR);
    assert_sent(5, SPINEL_CMD_PROP_VALUE_SET, SPINEL_PROP_MAC_PROMISCUOUS_MODE);
    assert_sent(6, SPINEL_CMD_PROP_VALUE_SET, SPINEL_PROP_MAC_SRC_MATCH_ENABLED);
    assert_sent(7, SPINEL_CMD_PROP_VALUE_SET, SPINEL_PROP_MAC_RX_ON_WHEN_IDLE_MODE);
}

TEST_CASE("configure: encodes each value as RadioSpinel does", "[zigbee]")
{
    uint8_t u8 = 0;
    int8_t i8 = 0;
    uint16_t u16 = 0;
    bool b = false;
    spinel_eui64_t eui;

    attached(FRAMES_C6_RADIO_CAPS);
    zigbee_spinel_configure(&client, &CONFIG);
    req_t r = sent(0); // RadioSpinel::Receive: "C"
    TEST_ASSERT_EQUAL(1, r.len);
    spinel_datatype_unpack(r.data, r.len, "C", &u8);
    TEST_ASSERT_EQUAL(15, u8);
    r = sent(1); // SetTransmitPower: "c"
    TEST_ASSERT_EQUAL(1, r.len);
    spinel_datatype_unpack(r.data, r.len, "c", &i8);
    TEST_ASSERT_EQUAL(8, i8);
    r = sent(2); // SetPanId: "S"
    TEST_ASSERT_EQUAL(2, r.len);
    spinel_datatype_unpack(r.data, r.len, "S", &u16);
    TEST_ASSERT_EQUAL_HEX16(0x1a62, u16);
    r = sent(3); // SetShortAddress: "S"
    TEST_ASSERT_EQUAL(2, r.len);
    spinel_datatype_unpack(r.data, r.len, "S", &u16);
    TEST_ASSERT_EQUAL_HEX16(0x0000, u16);
    r = sent(4); // SetExtendedAddress: "E", little-endian (Radio::SetExtendedAddress reverses)
    TEST_ASSERT_EQUAL(8, r.len);
    spinel_datatype_unpack_in_place(r.data, r.len, "E", &eui);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(CONFIG_EXT_REVERSED, eui.bytes, 8);
    r = sent(5); // SetPromiscuous: "C" OFF
    TEST_ASSERT_EQUAL(1, r.len);
    spinel_datatype_unpack(r.data, r.len, "C", &u8);
    TEST_ASSERT_EQUAL(SPINEL_MAC_PROMISCUOUS_MODE_OFF, u8);
    r = sent(6); // EnableSrcMatch: "b"
    TEST_ASSERT_EQUAL(1, r.len);
    spinel_datatype_unpack(r.data, r.len, "b", &b);
    TEST_ASSERT_TRUE(b);
    r = sent(7); // SetRxOnWhenIdle: "b"
    TEST_ASSERT_EQUAL(1, r.len);
    spinel_datatype_unpack(r.data, r.len, "b", &b);
    TEST_ASSERT_TRUE(b);
}

TEST_CASE("configure promiscuous: sends MAC_PROMISCUOUS_MODE NETWORK", "[zigbee]")
{
    zigbee_radio_config_t config = CONFIG;
    uint8_t mode = 0;

    attached(FRAMES_C6_RADIO_CAPS);
    config.promiscuous = true;
    zigbee_spinel_configure(&client, &config);
    req_t r = sent(5);
    spinel_datatype_unpack(r.data, r.len, "C", &mode);
    TEST_ASSERT_EQUAL(SPINEL_MAC_PROMISCUOUS_MODE_NETWORK, mode);
}

TEST_CASE("configure on an RCP without rx-on-when-idle: leaves MAC_RX_ON_WHEN_IDLE_MODE out", "[zigbee]")
{
    attached(FRAMES_CC2652P_RADIO_CAPS);
    TEST_ASSERT_EQUAL(0, zigbee_spinel_configure(&client, &CONFIG));
    TEST_ASSERT_EQUAL(7, rcp.sent_count);
    TEST_ASSERT_EQUAL(0, count_sent(SPINEL_CMD_PROP_VALUE_SET, SPINEL_PROP_MAC_RX_ON_WHEN_IDLE_MODE));
}

TEST_CASE("configure: stores the configuration for replay", "[zigbee]")
{
    uint16_t pan = 0;

    attached(FRAMES_C6_RADIO_CAPS);
    zigbee_spinel_configure(&client, &CONFIG);
    rcp.sent_count = 0;
    zigbee_spinel_attach(&client);
    assert_sent(10, SPINEL_CMD_PROP_VALUE_SET, SPINEL_PROP_MAC_15_4_PANID);
    req_t r = sent(10);
    spinel_datatype_unpack(r.data, r.len, "S", &pan);
    TEST_ASSERT_EQUAL_HEX16(0x1a62, pan);
}

TEST_CASE("configure on an RCP error status: returns nonzero", "[zigbee]")
{
    attached(FRAMES_C6_RADIO_CAPS);
    rcp.fail_key = SPINEL_PROP_PHY_TX_POWER;
    rcp.fail_status = SPINEL_STATUS_INVALID_ARGUMENT;
    TEST_ASSERT_NOT_EQUAL(0, zigbee_spinel_configure(&client, &CONFIG));
}

TEST_CASE("configure on an RCP error status: stops at the refused property", "[zigbee]")
{
    attached(FRAMES_C6_RADIO_CAPS);
    rcp.fail_key = SPINEL_PROP_PHY_TX_POWER;
    rcp.fail_status = SPINEL_STATUS_INVALID_ARGUMENT;
    zigbee_spinel_configure(&client, &CONFIG);
    TEST_ASSERT_EQUAL(2, rcp.sent_count);
}

TEST_CASE("configure with a reply for another property: returns nonzero", "[zigbee]")
{
    attached(FRAMES_C6_RADIO_CAPS);
    rcp.wrong_key = SPINEL_PROP_PHY_CHAN;
    TEST_ASSERT_NOT_EQUAL(0, zigbee_spinel_configure(&client, &CONFIG));
}

/* ---- set_channel / set_promiscuous ---- */

TEST_CASE("set_channel: sends PHY_CHAN and returns 0", "[zigbee]")
{
    uint8_t ch = 0;

    attached(FRAMES_C6_RADIO_CAPS);
    TEST_ASSERT_EQUAL(0, zigbee_spinel_set_channel(&client, 25));
    TEST_ASSERT_EQUAL(1, rcp.sent_count);
    assert_sent(0, SPINEL_CMD_PROP_VALUE_SET, SPINEL_PROP_PHY_CHAN);
    spinel_datatype_unpack(sent(0).data, sent(0).len, "C", &ch);
    TEST_ASSERT_EQUAL(25, ch);
}

TEST_CASE("set_channel on an RCP error status: returns nonzero", "[zigbee]")
{
    attached(FRAMES_C6_RADIO_CAPS);
    rcp.fail_key = SPINEL_PROP_PHY_CHAN;
    rcp.fail_status = SPINEL_STATUS_INVALID_ARGUMENT;
    TEST_ASSERT_NOT_EQUAL(0, zigbee_spinel_set_channel(&client, 27));
}

TEST_CASE("set_channel: the stored configuration replays the new channel", "[zigbee]")
{
    uint8_t ch = 0;

    attached(FRAMES_C6_RADIO_CAPS);
    zigbee_spinel_configure(&client, &CONFIG);
    zigbee_spinel_set_channel(&client, 20);
    rcp.sent_count = 0;
    zigbee_spinel_attach(&client);
    assert_sent(8, SPINEL_CMD_PROP_VALUE_SET, SPINEL_PROP_PHY_CHAN);
    spinel_datatype_unpack(sent(8).data, sent(8).len, "C", &ch);
    TEST_ASSERT_EQUAL(20, ch);
}

TEST_CASE("set_promiscuous: sends MAC_PROMISCUOUS_MODE NETWORK and returns 0", "[zigbee]")
{
    uint8_t mode = 0;

    attached(FRAMES_C6_RADIO_CAPS);
    TEST_ASSERT_EQUAL(0, zigbee_spinel_set_promiscuous(&client, true));
    assert_sent(0, SPINEL_CMD_PROP_VALUE_SET, SPINEL_PROP_MAC_PROMISCUOUS_MODE);
    spinel_datatype_unpack(sent(0).data, sent(0).len, "C", &mode);
    TEST_ASSERT_EQUAL(SPINEL_MAC_PROMISCUOUS_MODE_NETWORK, mode);
}

TEST_CASE("set_promiscuous: the stored configuration replays it", "[zigbee]")
{
    uint8_t mode = 0;

    attached(FRAMES_C6_RADIO_CAPS);
    zigbee_spinel_configure(&client, &CONFIG);
    zigbee_spinel_set_promiscuous(&client, true);
    rcp.sent_count = 0;
    zigbee_spinel_attach(&client);
    assert_sent(13, SPINEL_CMD_PROP_VALUE_SET, SPINEL_PROP_MAC_PROMISCUOUS_MODE);
    spinel_datatype_unpack(sent(13).data, sent(13).len, "C", &mode);
    TEST_ASSERT_EQUAL(SPINEL_MAC_PROMISCUOUS_MODE_NETWORK, mode);
}

/* ---- source match ---- */

TEST_CASE("src_match_clear: empties both RCP tables with empty sets", "[zigbee]")
{
    attached(FRAMES_C6_RADIO_CAPS);
    zigbee_spinel_src_match_clear(&client);
    TEST_ASSERT_EQUAL(2, rcp.sent_count);
    assert_sent(0, SPINEL_CMD_PROP_VALUE_SET, SPINEL_PROP_MAC_SRC_MATCH_SHORT_ADDRESSES);
    TEST_ASSERT_EQUAL(0, sent(0).len);
    assert_sent(1, SPINEL_CMD_PROP_VALUE_SET, SPINEL_PROP_MAC_SRC_MATCH_EXTENDED_ADDRESSES);
    TEST_ASSERT_EQUAL(0, sent(1).len);
}

TEST_CASE("src_match_add_short: inserts the address and returns 0", "[zigbee]")
{
    uint16_t addr = 0;

    attached(FRAMES_C6_RADIO_CAPS);
    TEST_ASSERT_EQUAL(0, zigbee_spinel_src_match_add_short(&client, 0x1234));
    assert_sent(0, SPINEL_CMD_PROP_VALUE_INSERT, SPINEL_PROP_MAC_SRC_MATCH_SHORT_ADDRESSES);
    TEST_ASSERT_EQUAL(2, sent(0).len);
    spinel_datatype_unpack(sent(0).data, sent(0).len, "S", &addr);
    TEST_ASSERT_EQUAL_HEX16(0x1234, addr);
}

TEST_CASE("src_match_add_ext: inserts the address in the RCP's byte order and returns 0", "[zigbee]")
{
    attached(FRAMES_C6_RADIO_CAPS);
    TEST_ASSERT_EQUAL(0, zigbee_spinel_src_match_add_ext(&client, EXT_A));
    assert_sent(0, SPINEL_CMD_PROP_VALUE_INSERT, SPINEL_PROP_MAC_SRC_MATCH_EXTENDED_ADDRESSES);
    TEST_ASSERT_EQUAL(8, sent(0).len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(EXT_A_REVERSED, sent(0).data, 8);
}

TEST_CASE("src_match_clear: empties the stored copy", "[zigbee]")
{
    attached(FRAMES_C6_RADIO_CAPS);
    zigbee_spinel_src_match_add_short(&client, 0x1234);
    zigbee_spinel_src_match_add_ext(&client, EXT_A);
    zigbee_spinel_src_match_clear(&client);
    rcp.sent_count = 0;
    zigbee_spinel_attach(&client);
    TEST_ASSERT_EQUAL(0, count_sent(SPINEL_CMD_PROP_VALUE_INSERT, SPINEL_PROP_MAC_SRC_MATCH_SHORT_ADDRESSES));
    TEST_ASSERT_EQUAL(0, count_sent(SPINEL_CMD_PROP_VALUE_INSERT, SPINEL_PROP_MAC_SRC_MATCH_EXTENDED_ADDRESSES));
}

TEST_CASE("src_match add of an entry already stored: replayed once", "[zigbee]")
{
    attached(FRAMES_C6_RADIO_CAPS);
    zigbee_spinel_src_match_add_short(&client, 0x1234);
    zigbee_spinel_src_match_add_short(&client, 0x1234);
    rcp.sent_count = 0;
    zigbee_spinel_attach(&client);
    TEST_ASSERT_EQUAL(1, count_sent(SPINEL_CMD_PROP_VALUE_INSERT, SPINEL_PROP_MAC_SRC_MATCH_SHORT_ADDRESSES));
}

TEST_CASE("src_match add the RCP refuses: returns nonzero", "[zigbee]")
{
    attached(FRAMES_C6_RADIO_CAPS);
    rcp.short_limit = 1;
    rcp.ext_limit = 1;
    TEST_ASSERT_EQUAL(0, zigbee_spinel_src_match_add_short(&client, 0x0001));
    TEST_ASSERT_NOT_EQUAL(0, zigbee_spinel_src_match_add_short(&client, 0x0002));
    TEST_ASSERT_EQUAL(0, zigbee_spinel_src_match_add_ext(&client, EXT_A));
    TEST_ASSERT_NOT_EQUAL(0, zigbee_spinel_src_match_add_ext(&client, EXT_B));
}

TEST_CASE("frame-pending table full: an add the RCP refuses is not replayed", "[zigbee]")
{
    uint16_t addr = 0;

    attached(FRAMES_C6_RADIO_CAPS);
    rcp.short_limit = 2;
    rcp.ext_limit = 1;
    zigbee_spinel_src_match_clear(&client);
    TEST_ASSERT_EQUAL(0, zigbee_spinel_src_match_add_short(&client, 0x0001));
    TEST_ASSERT_EQUAL(0, zigbee_spinel_src_match_add_short(&client, 0x0002));
    TEST_ASSERT_NOT_EQUAL(0, zigbee_spinel_src_match_add_short(&client, 0x0003));
    TEST_ASSERT_EQUAL(0, zigbee_spinel_src_match_add_ext(&client, EXT_A));
    TEST_ASSERT_NOT_EQUAL(0, zigbee_spinel_src_match_add_ext(&client, EXT_B));
    rcp.sent_count = 0;

    zigbee_spinel_attach(&client);
    TEST_ASSERT_EQUAL(2, count_sent(SPINEL_CMD_PROP_VALUE_INSERT, SPINEL_PROP_MAC_SRC_MATCH_SHORT_ADDRESSES));
    TEST_ASSERT_EQUAL(1, count_sent(SPINEL_CMD_PROP_VALUE_INSERT, SPINEL_PROP_MAC_SRC_MATCH_EXTENDED_ADDRESSES));
    for (size_t i = 0; i < rcp.sent_count; i++) {
        req_t r = sent(i);
        if (r.cmd == SPINEL_CMD_PROP_VALUE_INSERT && r.key == SPINEL_PROP_MAC_SRC_MATCH_SHORT_ADDRESSES) {
            spinel_datatype_unpack(r.data, r.len, "S", &addr);
            TEST_ASSERT_NOT_EQUAL(0x0003, addr);
        }
        if (r.cmd == SPINEL_CMD_PROP_VALUE_INSERT && r.key == SPINEL_PROP_MAC_SRC_MATCH_EXTENDED_ADDRESSES) {
            TEST_ASSERT_EQUAL_UINT8_ARRAY(EXT_A_REVERSED, r.data, 8);
        }
    }
    TEST_ASSERT_EQUAL(1, got.ready_count);
    TEST_ASSERT_TRUE(got.compatible);
}

TEST_CASE("frame-pending table full: after a clear the table takes entries again", "[zigbee]")
{
    attached(FRAMES_C6_RADIO_CAPS);
    rcp.short_limit = 1;
    zigbee_spinel_src_match_add_short(&client, 0x0001);
    TEST_ASSERT_NOT_EQUAL(0, zigbee_spinel_src_match_add_short(&client, 0x0002));
    zigbee_spinel_src_match_clear(&client);
    TEST_ASSERT_EQUAL(0, zigbee_spinel_src_match_add_short(&client, 0x0002));
    rcp.sent_count = 0;
    zigbee_spinel_attach(&client);
    TEST_ASSERT_EQUAL(1, count_sent(SPINEL_CMD_PROP_VALUE_INSERT, SPINEL_PROP_MAC_SRC_MATCH_SHORT_ADDRESSES));
}

TEST_CASE("frame-pending table full on replay: an entry the new RCP refuses leaves the stored copy", "[zigbee]")
{
    attached(FRAMES_C6_RADIO_CAPS);
    zigbee_spinel_src_match_add_short(&client, 0x0001);
    zigbee_spinel_src_match_add_short(&client, 0x0002);
    rcp.short_limit = 1; // a smaller table after the reset
    zigbee_spinel_attach(&client);
    rcp.short_limit = 0;
    rcp.sent_count = 0;
    zigbee_spinel_attach(&client);
    TEST_ASSERT_EQUAL(1, count_sent(SPINEL_CMD_PROP_VALUE_INSERT, SPINEL_PROP_MAC_SRC_MATCH_SHORT_ADDRESSES));
}

/* ---- transmit ---- */

static const uint8_t PSDU[12] = {0x41, 0x88, 0x07, 0x62, 0x1a, 0xff, 0xff, 0x00, 0x00, 0x09, 0x12, 0x34};

typedef struct {
    const uint8_t *frame;
    unsigned frame_len;
    uint8_t channel, backoffs, retries;
    bool csma, header_updated, retx, security_processed;
    uint32_t delay, delay_base;
    uint8_t rx_channel;
    int8_t power;
} raw_tx_t;

/* RadioSpinel::Transmit's STREAM_RAW layout. */
static raw_tx_t sent_raw(size_t i)
{
    raw_tx_t t = {0};
    req_t r = sent(i);

    TEST_ASSERT_EQUAL_UINT(SPINEL_CMD_PROP_VALUE_SET, r.cmd);
    TEST_ASSERT_EQUAL_UINT(SPINEL_PROP_STREAM_RAW, r.key);
    TEST_ASSERT_EQUAL((spinel_ssize_t)r.len,
                      spinel_datatype_unpack(r.data, r.len, "dCCCbbbbLLCc", &t.frame, &t.frame_len, &t.channel,
                                             &t.backoffs, &t.retries, &t.csma, &t.header_updated, &t.retx,
                                             &t.security_processed, &t.delay, &t.delay_base, &t.rx_channel,
                                             &t.power));
    return t;
}

static void answer_tx(unsigned status)
{
    uint8_t buf[96];
    uint8_t tid = 0;

    for (size_t i = rcp.sent_count; i-- > 0;) {
        if (sent(i).key == SPINEL_PROP_STREAM_RAW) {
            tid = sent_tid(i);
            break;
        }
    }
    TEST_ASSERT_NOT_EQUAL(0, tid);
    deliver(buf, frames_tx_done(buf, sizeof(buf), tid, status));
}

TEST_CASE("transmit: sends one STREAM_RAW frame with the PSDU and channel, CSMA, retries and backoffs", "[zigbee]")
{
    attached(FRAMES_C6_RADIO_CAPS);
    TEST_ASSERT_EQUAL(0, zigbee_spinel_transmit(&client, PSDU, sizeof(PSDU), 15, true, 3, 4));
    TEST_ASSERT_EQUAL(1, rcp.sent_count);
    raw_tx_t t = sent_raw(0);
    TEST_ASSERT_EQUAL(sizeof(PSDU) + 2, t.frame_len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(PSDU, t.frame, sizeof(PSDU));
    TEST_ASSERT_EQUAL(15, t.channel);
    TEST_ASSERT_TRUE(t.csma);
    TEST_ASSERT_EQUAL(3, t.retries);
    TEST_ASSERT_EQUAL(4, t.backoffs);
    TEST_ASSERT_EQUAL(15, t.rx_channel);
    TEST_ASSERT_NOT_EQUAL(0, sent_tid(0));
}

TEST_CASE("transmit: marks the frame secured and final, not a retransmission, undelayed, default power",
          "[zigbee]")
{
    attached(FRAMES_C6_RADIO_CAPS);
    zigbee_spinel_transmit(&client, PSDU, sizeof(PSDU), 20, false, 0, 0);
    raw_tx_t t = sent_raw(0);
    TEST_ASSERT_TRUE(t.header_updated);
    TEST_ASSERT_TRUE(t.security_processed);
    TEST_ASSERT_FALSE(t.retx);
    TEST_ASSERT_FALSE(t.csma);
    TEST_ASSERT_EQUAL_UINT32(0, t.delay);
    TEST_ASSERT_EQUAL_UINT32(0, t.delay_base);
    TEST_ASSERT_EQUAL(127, t.power); // OT_RADIO_POWER_INVALID: no per-frame power
}

TEST_CASE("transmit: appends the 802.15.4 FCS", "[zigbee]")
{
    attached(FRAMES_C6_RADIO_CAPS);
    // CRC-16/KERMIT (the 802.15.4 FCS) of "123456789" is 0x2189, sent little-endian.
    zigbee_spinel_transmit(&client, (const uint8_t *)"123456789", 9, 15, true, 3, 4);
    raw_tx_t t = sent_raw(0);
    TEST_ASSERT_EQUAL(11, t.frame_len);
    TEST_ASSERT_EQUAL_HEX8(0x89, t.frame[9]);
    TEST_ASSERT_EQUAL_HEX8(0x21, t.frame[10]);
}

TEST_CASE("transmit answered OK: reports acked", "[zigbee]")
{
    attached(FRAMES_C6_RADIO_CAPS);
    zigbee_spinel_transmit(&client, PSDU, sizeof(PSDU), 15, true, 3, 4);
    answer_tx(SPINEL_STATUS_OK);
    TEST_ASSERT_EQUAL(1, got.tx_count);
    TEST_ASSERT_EQUAL(ZIGBEE_TX_ACKED, got.tx);
}

TEST_CASE("transmit answered NO_ACK: reports no ack", "[zigbee]")
{
    attached(FRAMES_C6_RADIO_CAPS);
    zigbee_spinel_transmit(&client, PSDU, sizeof(PSDU), 15, true, 3, 4);
    answer_tx(SPINEL_STATUS_NO_ACK);
    TEST_ASSERT_EQUAL(ZIGBEE_TX_NO_ACK, got.tx);
}

TEST_CASE("transmit answered CCA_FAILURE: reports channel access failure", "[zigbee]")
{
    attached(FRAMES_C6_RADIO_CAPS);
    zigbee_spinel_transmit(&client, PSDU, sizeof(PSDU), 15, true, 3, 4);
    answer_tx(SPINEL_STATUS_CCA_FAILURE);
    TEST_ASSERT_EQUAL(ZIGBEE_TX_CCA, got.tx);
}

TEST_CASE("transmit answered with OpenThread's ABORT: reports aborted", "[zigbee]")
{
    attached(FRAMES_C6_RADIO_CAPS);
    zigbee_spinel_transmit(&client, PSDU, sizeof(PSDU), 15, true, 3, 4);
    answer_tx(FRAMES_STATUS_ABORT);
    TEST_ASSERT_EQUAL(ZIGBEE_TX_ABORTED, got.tx);
}

TEST_CASE("transmit answered FAILURE: reports failed", "[zigbee]")
{
    attached(FRAMES_C6_RADIO_CAPS);
    zigbee_spinel_transmit(&client, PSDU, sizeof(PSDU), 15, true, 3, 4);
    answer_tx(SPINEL_STATUS_FAILURE);
    TEST_ASSERT_EQUAL(ZIGBEE_TX_FAILED, got.tx);
}

TEST_CASE("transmit refused by the RCP at once: reports failed", "[zigbee]")
{
    uint8_t buf[32];

    attached(FRAMES_C6_RADIO_CAPS);
    zigbee_spinel_transmit(&client, PSDU, sizeof(PSDU), 15, true, 3, 4);
    deliver(buf, frames_tx_refused(buf, sizeof(buf), sent_tid(0), SPINEL_STATUS_INVALID_STATE));
    TEST_ASSERT_EQUAL(1, got.tx_count);
    TEST_ASSERT_EQUAL(ZIGBEE_TX_FAILED, got.tx);
}

TEST_CASE("transmit with a frame in flight: returns nonzero without sending", "[zigbee]")
{
    attached(FRAMES_C6_RADIO_CAPS);
    TEST_ASSERT_EQUAL(0, zigbee_spinel_transmit(&client, PSDU, sizeof(PSDU), 15, true, 3, 4));
    TEST_ASSERT_NOT_EQUAL(0, zigbee_spinel_transmit(&client, PSDU, sizeof(PSDU), 15, true, 3, 4));
    TEST_ASSERT_EQUAL(1, rcp.sent_count);
}

TEST_CASE("transmit after the answer: the next frame is sent", "[zigbee]")
{
    attached(FRAMES_C6_RADIO_CAPS);
    zigbee_spinel_transmit(&client, PSDU, sizeof(PSDU), 15, true, 3, 4);
    answer_tx(SPINEL_STATUS_OK);
    TEST_ASSERT_EQUAL(0, zigbee_spinel_transmit(&client, PSDU, sizeof(PSDU), 15, true, 3, 4));
    TEST_ASSERT_EQUAL(2, rcp.sent_count);
}

TEST_CASE("transmit longer than 127 bytes with FCS: returns nonzero without sending", "[zigbee]")
{
    uint8_t big[126] = {0x41, 0x88};

    attached(FRAMES_C6_RADIO_CAPS);
    TEST_ASSERT_NOT_EQUAL(0, zigbee_spinel_transmit(&client, big, 126, 15, true, 3, 4));
    TEST_ASSERT_EQUAL(0, rcp.sent_count);
    TEST_ASSERT_EQUAL(0, zigbee_spinel_transmit(&client, big, 125, 15, true, 3, 4));
    TEST_ASSERT_EQUAL(127, sent_raw(0).frame_len);
}

TEST_CASE("transmit before attach: returns nonzero without sending", "[zigbee]")
{
    setup(FRAMES_C6_RADIO_CAPS);
    TEST_ASSERT_NOT_EQUAL(0, zigbee_spinel_transmit(&client, PSDU, sizeof(PSDU), 15, true, 3, 4));
    TEST_ASSERT_EQUAL(0, rcp.sent_count);
}

TEST_CASE("transmit the channel refuses to send: returns nonzero and the next transmit is accepted", "[zigbee]")
{
    attached(FRAMES_C6_RADIO_CAPS);
    rcp.send_result = -1;
    TEST_ASSERT_NOT_EQUAL(0, zigbee_spinel_transmit(&client, PSDU, sizeof(PSDU), 15, true, 3, 4));
    rcp.send_result = 0;
    TEST_ASSERT_EQUAL(0, zigbee_spinel_transmit(&client, PSDU, sizeof(PSDU), 15, true, 3, 4));
}

TEST_CASE("transmit on an RCP without rx-on-when-idle: puts the radio back to receive after the answer",
          "[zigbee]")
{
    attached(FRAMES_CC2652P_RADIO_CAPS);
    zigbee_spinel_transmit(&client, PSDU, sizeof(PSDU), 15, true, 3, 4);
    TEST_ASSERT_EQUAL(1, rcp.sent_count);
    answer_tx(SPINEL_STATUS_NO_ACK);
    TEST_ASSERT_EQUAL(2, rcp.sent_count);
    assert_sent(1, SPINEL_CMD_PROP_VALUE_SET, SPINEL_PROP_MAC_RAW_STREAM_ENABLED);
    TEST_ASSERT_TRUE(sent_bool(1));
    TEST_ASSERT_EQUAL(1, got.tx_count);
}

TEST_CASE("transmit on an RCP with rx-on-when-idle: sends nothing after the answer", "[zigbee]")
{
    attached(FRAMES_C6_RADIO_CAPS);
    zigbee_spinel_transmit(&client, PSDU, sizeof(PSDU), 15, true, 3, 4);
    answer_tx(SPINEL_STATUS_OK);
    TEST_ASSERT_EQUAL(1, rcp.sent_count);
}

TEST_CASE("transmit with no answer for 5 s: asks for a channel reset once", "[zigbee]")
{
    attached(FRAMES_C6_RADIO_CAPS);
    zigbee_spinel_transmit(&client, PSDU, sizeof(PSDU), 15, true, 3, 4);
    rcp.now += ZIGBEE_SPINEL_TX_TIMEOUT_US - 1;
    zigbee_spinel_on_tick(&client);
    TEST_ASSERT_EQUAL(0, rcp.resets_requested);
    rcp.now += 1;
    zigbee_spinel_on_tick(&client);
    TEST_ASSERT_EQUAL(1, rcp.resets_requested);
    zigbee_spinel_on_tick(&client);
    TEST_ASSERT_EQUAL(1, rcp.resets_requested);
}

TEST_CASE("transmit re-arm with no reply for 2000 ms: asks for a channel reset", "[zigbee]")
{
    attached(FRAMES_CC2652P_RADIO_CAPS);
    rcp.silent_key = SPINEL_PROP_MAC_RAW_STREAM_ENABLED;
    zigbee_spinel_transmit(&client, PSDU, sizeof(PSDU), 15, true, 3, 4);
    answer_tx(SPINEL_STATUS_OK);
    rcp.now += ZIGBEE_SPINEL_REQUEST_TIMEOUT_US;
    zigbee_spinel_on_tick(&client);
    TEST_ASSERT_EQUAL(1, rcp.resets_requested);
}

TEST_CASE("transmit answered while a property request waits: the answer reaches the sink from inside wait",
          "[zigbee]")
{
    uint8_t buf[96];

    attached(FRAMES_C6_RADIO_CAPS);
    TEST_ASSERT_EQUAL(0, zigbee_spinel_transmit(&client, PSDU, sizeof(PSDU), 15, true, 3, 4));
    rcp.inject[0].len = frames_tx_done(rcp.inject[0].buf, sizeof(rcp.inject[0].buf), sent_tid(0), SPINEL_STATUS_OK);
    rcp.inject_count = 1;
    (void)buf;
    TEST_ASSERT_EQUAL(0, zigbee_spinel_set_channel(&client, 20));
    TEST_ASSERT_EQUAL(1, got.tx_count);
    TEST_ASSERT_EQUAL(ZIGBEE_TX_ACKED, got.tx);
    TEST_ASSERT_NOT_EQUAL(sent_tid(0), sent_tid(1)); // the two transactions do not share a TID
}

/* ---- energy scan ---- */

TEST_CASE("energy_scan: sets MAC_SCAN_MASK, MAC_SCAN_PERIOD and MAC_SCAN_STATE ENERGY", "[zigbee]")
{
    uint16_t period = 0;
    uint8_t state = 0;

    attached(FRAMES_C6_RADIO_CAPS);
    TEST_ASSERT_EQUAL(0, zigbee_spinel_energy_scan(&client, 17, 250));
    TEST_ASSERT_EQUAL(3, rcp.sent_count);
    assert_sent(0, SPINEL_CMD_PROP_VALUE_SET, SPINEL_PROP_MAC_SCAN_MASK);
    TEST_ASSERT_EQUAL(1, sent(0).len); // "D": the bare channel byte
    TEST_ASSERT_EQUAL(17, sent(0).data[0]);
    assert_sent(1, SPINEL_CMD_PROP_VALUE_SET, SPINEL_PROP_MAC_SCAN_PERIOD);
    spinel_datatype_unpack(sent(1).data, sent(1).len, "S", &period);
    TEST_ASSERT_EQUAL(250, period);
    assert_sent(2, SPINEL_CMD_PROP_VALUE_SET, SPINEL_PROP_MAC_SCAN_STATE);
    spinel_datatype_unpack(sent(2).data, sent(2).len, "C", &state);
    TEST_ASSERT_EQUAL(SPINEL_SCAN_STATE_ENERGY, state);
}

TEST_CASE("energy_scan: reports the peak RSSI from MAC_ENERGY_SCAN_RESULT", "[zigbee]")
{
    uint8_t buf[32];

    attached(FRAMES_C6_RADIO_CAPS);
    zigbee_spinel_energy_scan(&client, 17, 250);
    TEST_ASSERT_EQUAL(0, got.scan_count);
    deliver(buf, frames_energy_result(buf, sizeof(buf), 17, -71));
    TEST_ASSERT_EQUAL(1, got.scan_count);
    TEST_ASSERT_EQUAL(-71, got.scan_rssi);
}

TEST_CASE("energy_scan refused by the RCP: returns nonzero", "[zigbee]")
{
    attached(FRAMES_C6_RADIO_CAPS);
    rcp.fail_key = SPINEL_PROP_MAC_SCAN_STATE;
    rcp.fail_status = SPINEL_STATUS_INVALID_STATE;
    TEST_ASSERT_NOT_EQUAL(0, zigbee_spinel_energy_scan(&client, 17, 250));
}

TEST_CASE("energy_scan while one runs: returns nonzero without sending", "[zigbee]")
{
    attached(FRAMES_C6_RADIO_CAPS);
    zigbee_spinel_energy_scan(&client, 17, 250);
    TEST_ASSERT_NOT_EQUAL(0, zigbee_spinel_energy_scan(&client, 18, 250));
    TEST_ASSERT_EQUAL(3, rcp.sent_count);
}

TEST_CASE("energy_scan with no result: asks for a channel reset after the duration and 2000 ms", "[zigbee]")
{
    attached(FRAMES_C6_RADIO_CAPS);
    zigbee_spinel_energy_scan(&client, 17, 250);
    rcp.now += 250 * 1000LL + ZIGBEE_SPINEL_SCAN_GRACE_US - 1;
    zigbee_spinel_on_tick(&client);
    TEST_ASSERT_EQUAL(0, rcp.resets_requested);
    rcp.now += 1;
    zigbee_spinel_on_tick(&client);
    TEST_ASSERT_EQUAL(1, rcp.resets_requested);
}

TEST_CASE("energy_scan before attach: returns nonzero without sending", "[zigbee]")
{
    setup(FRAMES_C6_RADIO_CAPS);
    TEST_ASSERT_NOT_EQUAL(0, zigbee_spinel_energy_scan(&client, 17, 250));
    TEST_ASSERT_EQUAL(0, rcp.sent_count);
}

/* ---- received frame ---- */

static const uint8_t RX_PSDU[10] = {0x41, 0x88, 0x2a, 0x62, 0x1a, 0x00, 0x00, 0x34, 0x12, 0x77}; // ... + 2 FCS

static void deliver_rx(uint8_t iid, const uint8_t *psdu, size_t len, unsigned rx_error)
{
    uint8_t buf[200];
    zigbee_spinel_on_frame(&client, buf, frames_stream_raw_rx(buf, sizeof(buf), iid, psdu, len, -55, 15, 180, 999,
                                                              rx_error), 4242);
}

TEST_CASE("received STREAM_RAW frame: delivers the PSDU without FCS", "[zigbee]")
{
    attached(FRAMES_C6_RADIO_CAPS);
    deliver_rx(0, RX_PSDU, sizeof(RX_PSDU), 0);
    TEST_ASSERT_EQUAL(1, got.rx_count);
    TEST_ASSERT_EQUAL(sizeof(RX_PSDU) - 2, got.psdu_len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(RX_PSDU, got.psdu, sizeof(RX_PSDU) - 2);
}

TEST_CASE("received STREAM_RAW frame: delivers channel, RSSI and LQI", "[zigbee]")
{
    attached(FRAMES_C6_RADIO_CAPS);
    deliver_rx(0, RX_PSDU, sizeof(RX_PSDU), 0);
    TEST_ASSERT_EQUAL(15, got.channel);
    TEST_ASSERT_EQUAL(-55, got.rssi);
    TEST_ASSERT_EQUAL(180, got.lqi);
}

TEST_CASE("received STREAM_RAW frame: delivers the mux's receive time", "[zigbee]")
{
    attached(FRAMES_C6_RADIO_CAPS);
    deliver_rx(0, RX_PSDU, sizeof(RX_PSDU), 0);
    TEST_ASSERT_EQUAL_UINT64(4242, got.time_us); // not the RCP's own timestamp (999)
}

TEST_CASE("received STREAM_RAW frame with a receive error: dropped", "[zigbee]")
{
    attached(FRAMES_C6_RADIO_CAPS);
    deliver_rx(0, RX_PSDU, sizeof(RX_PSDU), 13 /* OT_ERROR_NO_FRAME_RECEIVED... any nonzero */);
    TEST_ASSERT_EQUAL(0, got.rx_count);
}

TEST_CASE("received STREAM_RAW frame shorter than its FCS: dropped", "[zigbee]")
{
    attached(FRAMES_C6_RADIO_CAPS);
    deliver_rx(0, RX_PSDU, 1, 0);
    TEST_ASSERT_EQUAL(0, got.rx_count);
}

TEST_CASE("received STREAM_RAW frame truncated: dropped", "[zigbee]")
{
    uint8_t buf[200];

    attached(FRAMES_C6_RADIO_CAPS);
    size_t n = frames_stream_raw_rx(buf, sizeof(buf), 0, RX_PSDU, sizeof(RX_PSDU), -55, 15, 180, 999, 0);
    zigbee_spinel_on_frame(&client, buf, 3 + 2 + 6, 1); // header, command, key, length and half the PSDU
    (void)n;
    TEST_ASSERT_EQUAL(0, got.rx_count);
}

TEST_CASE("received STREAM_RAW frame for another interface: ignored", "[zigbee]")
{
    attached(FRAMES_C6_RADIO_CAPS);
    deliver_rx(1, RX_PSDU, sizeof(RX_PSDU), 0);
    TEST_ASSERT_EQUAL(0, got.rx_count);
}

TEST_CASE("received STREAM_RAW frame before attach: dropped", "[zigbee]")
{
    setup(FRAMES_C6_RADIO_CAPS);
    deliver_rx(0, RX_PSDU, sizeof(RX_PSDU), 0);
    TEST_ASSERT_EQUAL(0, got.rx_count);
}

TEST_CASE("received STREAM_RAW frame while a property request waits: reaches the sink from inside wait",
          "[zigbee]")
{
    attached(FRAMES_C6_RADIO_CAPS);
    rcp.inject[0].len = frames_stream_raw_rx(rcp.inject[0].buf, sizeof(rcp.inject[0].buf), 0, RX_PSDU,
                                             sizeof(RX_PSDU), -55, 15, 180, 999, 0);
    rcp.inject_count = 1;
    TEST_ASSERT_EQUAL(0, zigbee_spinel_set_channel(&client, 20));
    TEST_ASSERT_EQUAL(1, got.rx_count);
}

TEST_CASE("received garbage: ignored", "[zigbee]")
{
    static const uint8_t junk[] = {0x00, 0x06, 0x71};

    attached(FRAMES_C6_RADIO_CAPS);
    zigbee_spinel_on_frame(&client, junk, sizeof(junk), 1); // no header flag
    zigbee_spinel_on_frame(&client, junk, 0, 1);
    TEST_ASSERT_EQUAL(0, got.rx_count);
    TEST_ASSERT_EQUAL(0, zigbee_spinel_set_channel(&client, 20));
}
