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
#define KEY_CMD_RESET (UINT32_MAX - 1) // event_key value: the CMD_RESET itself

/* What the board passes to the client from inside wait (the mux's events). */
typedef enum {
    EVENT_NONE,
    EVENT_RESET,       // zigbee_spinel_on_reset
    EVENT_GONE,        // zigbee_spinel_on_gone
    EVENT_GONE_ATTACH, // zigbee_spinel_on_gone, then zigbee_spinel_attach, in one wait
} event_t;

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
    unsigned silent_cmd; // 0: silent for every command on silent_key
    unsigned abort_key; // a request for it gets no reply; the next wait aborts it (Task 3's RESET/GONE)
    bool abort_pending;
    unsigned event_key;        // one-shot: a request for it gets no reply; the next wait delivers `event`
    unsigned event_reply_key;  // one-shot: a request for it is answered, and the wait that delivers the reply
    // delivers `event` right after it
    event_t event, event_pending;
    bool event_after_frame;
    unsigned reset_status_key; // one-shot: answered with an unsolicited reset status instead of its reply
    unsigned wrong_key; // answered with VALUE_IS of another property
    size_t short_limit, ext_limit; // 0 = unlimited
    size_t short_count, ext_count;

    int64_t step_us; // clock advance per delivered frame
    int send_result;
    int resets_requested;
    int reset_result; // what request_reset returns: nonzero when the channel cannot be reset now
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
        if (rcp.event_key == KEY_CMD_RESET) {
            rcp.event_key = UINT32_MAX;
            rcp.event_pending = rcp.event;
            return;
        }
        if (!rcp.reset_silent) {
            queue_frame(buf, frames_reset_status(buf, sizeof(buf), rcp.reset_iid, SPINEL_STATUS_RESET_POWER_ON));
        }
        return;
    }
    if (key == rcp.event_key) {
        rcp.event_key = UINT32_MAX;
        rcp.event_pending = rcp.event;
        return;
    }
    if (key == rcp.event_reply_key) {
        rcp.event_reply_key = UINT32_MAX;
        rcp.event_after_frame = true; // the reply is queued below, as usual
    }
    if (key == rcp.reset_status_key) {
        rcp.reset_status_key = UINT32_MAX; // the RCP rebooted on its own instead of answering
        queue_frame(buf, frames_reset_status(buf, sizeof(buf), 0, SPINEL_STATUS_RESET_SOFTWARE));
        return;
    }
    if (key == rcp.silent_key && (rcp.silent_cmd == 0 || cmd == rcp.silent_cmd)) {
        return;
    }
    if (key == rcp.abort_key) {
        rcp.abort_pending = true;
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

static int fake_request_reset(void *ctx)
{
    (void)ctx;
    rcp.resets_requested++;
    return rcp.reset_result;
}

static int64_t fake_now(void *ctx)
{
    (void)ctx;
    return rcp.now;
}

static void fire_event(void)
{
    event_t e = rcp.event_pending;

    rcp.event_pending = EVENT_NONE;
    if (e == EVENT_RESET) {
        zigbee_spinel_on_reset(&client);
    } else if (e != EVENT_NONE) {
        zigbee_spinel_on_gone(&client);
        if (e == EVENT_GONE_ATTACH) {
            zigbee_spinel_attach(&client);
        }
    }
}

static int fake_wait(void *ctx, int64_t until_us)
{
    (void)ctx;
    if (rcp.abort_pending) {
        rcp.abort_pending = false;
        zigbee_spinel_abort_wait(&client);
        return 0;
    }
    if (rcp.event_pending != EVENT_NONE) {
        fire_event();
        return 0;
    }
    if (rcp.q_head == rcp.q_tail) {
        if (until_us > rcp.now) {
            rcp.now = until_us;
        }
        return 1;
    }
    frame_t f = rcp.queue[rcp.q_head++ % MAX_QUEUE];
    rcp.now += rcp.step_us;
    zigbee_spinel_on_frame(&client, f.buf, f.len, rcp.now);
    if (rcp.event_after_frame && rcp.q_head == rcp.q_tail) {
        rcp.event_after_frame = false;
        rcp.event_pending = rcp.event;
        fire_event();
    }
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
    rcp.abort_key = UINT32_MAX;
    rcp.event_key = UINT32_MAX;
    rcp.event_reply_key = UINT32_MAX;
    rcp.reset_status_key = UINT32_MAX;
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
    TEST_ASSERT_EQUAL_UINT8_ARRAY(EXT_A, sent(19).data, 8);
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

TEST_CASE("src_match_add_ext: inserts the address in the order ziggurat gives it (otLinkRawSrcMatchAddExtEntry's, which reaches the RCP unreversed) and returns 0", "[zigbee]")
{
    attached(FRAMES_C6_RADIO_CAPS);
    TEST_ASSERT_EQUAL(0, zigbee_spinel_src_match_add_ext(&client, EXT_A));
    assert_sent(0, SPINEL_CMD_PROP_VALUE_INSERT, SPINEL_PROP_MAC_SRC_MATCH_EXTENDED_ADDRESSES);
    TEST_ASSERT_EQUAL(8, sent(0).len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(EXT_A, sent(0).data, 8);
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
            TEST_ASSERT_EQUAL_UINT8_ARRAY(EXT_A, r.data, 8);
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

TEST_CASE("transmit the board refuses to send: returns ZIGBEE_SPINEL_ERR_LOCAL and asks for a channel reset once",
          "[zigbee]")
{
    attached(FRAMES_C6_RADIO_CAPS);
    rcp.send_result = -1; // the radio is gone under the board (C6 held in reset: hardware pass 2026-11-07)
    TEST_ASSERT_EQUAL(ZIGBEE_SPINEL_ERR_LOCAL, zigbee_spinel_transmit(&client, PSDU, sizeof(PSDU), 15, true, 3, 4));
    TEST_ASSERT_EQUAL(1, rcp.resets_requested);
    TEST_ASSERT_EQUAL(ZIGBEE_SPINEL_ERR_LOCAL, zigbee_spinel_transmit(&client, PSDU, sizeof(PSDU), 15, true, 3, 4));
    TEST_ASSERT_EQUAL(1, rcp.resets_requested);
}

TEST_CASE("requested reset with no RESET for ZIGBEE_SPINEL_RESET_RETRY_US: asks again, then after every interval",
          "[zigbee]")
{
    attached(FRAMES_C6_RADIO_CAPS);
    rcp.send_result = -1;
    zigbee_spinel_transmit(&client, PSDU, sizeof(PSDU), 15, true, 3, 4);
    TEST_ASSERT_EQUAL(1, rcp.resets_requested);
    rcp.now += ZIGBEE_SPINEL_RESET_RETRY_US - 1;
    zigbee_spinel_on_tick(&client);
    TEST_ASSERT_EQUAL(1, rcp.resets_requested);
    rcp.now += 1;
    zigbee_spinel_on_tick(&client);
    TEST_ASSERT_EQUAL(2, rcp.resets_requested);
    zigbee_spinel_on_tick(&client);
    TEST_ASSERT_EQUAL(2, rcp.resets_requested);
    rcp.now += ZIGBEE_SPINEL_RESET_RETRY_US;
    zigbee_spinel_on_tick(&client);
    TEST_ASSERT_EQUAL(3, rcp.resets_requested);
}

TEST_CASE("timed-out transmit whose reset brings no RESET: the reset is asked again after the retry interval",
          "[zigbee]")
{
    attached(FRAMES_C6_RADIO_CAPS);
    zigbee_spinel_transmit(&client, PSDU, sizeof(PSDU), 15, true, 3, 4);
    rcp.now += ZIGBEE_SPINEL_TX_TIMEOUT_US;
    zigbee_spinel_on_tick(&client);
    TEST_ASSERT_EQUAL(1, rcp.resets_requested);
    rcp.now += ZIGBEE_SPINEL_RESET_RETRY_US;
    zigbee_spinel_on_tick(&client);
    TEST_ASSERT_EQUAL(2, rcp.resets_requested);
}

TEST_CASE("RESET after a requested reset: no further reset is asked", "[zigbee]")
{
    attached(FRAMES_C6_RADIO_CAPS);
    rcp.send_result = -1;
    zigbee_spinel_transmit(&client, PSDU, sizeof(PSDU), 15, true, 3, 4);
    TEST_ASSERT_EQUAL(1, rcp.resets_requested);
    rcp.send_result = 0;
    zigbee_spinel_on_reset(&client);
    rcp.now += 3 * ZIGBEE_SPINEL_RESET_RETRY_US;
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

/* ---- timeouts in the replay, and aborts (fix round 1) ---- */

static void stored_three_shorts(void)
{
    attached(FRAMES_C6_RADIO_CAPS);
    zigbee_spinel_src_match_add_short(&client, 0x0001);
    zigbee_spinel_src_match_add_short(&client, 0x0002);
    zigbee_spinel_src_match_add_short(&client, 0x0003);
    rcp.sent_count = 0;
}

static void reattach_answering(void)
{
    rcp.silent_key = UINT32_MAX;
    rcp.silent_cmd = 0;
    rcp.abort_key = UINT32_MAX;
    rcp.resets_requested = 0;
    got.ready_count = 0;
    rcp.sent_count = 0;
    zigbee_spinel_attach(&client);
}

TEST_CASE("attach with a timeout during the source-match replay: keeps the stored table", "[zigbee]")
{
    stored_three_shorts();
    rcp.silent_key = SPINEL_PROP_MAC_SRC_MATCH_SHORT_ADDRESSES;
    rcp.silent_cmd = SPINEL_CMD_PROP_VALUE_INSERT; // the clear is answered, the first insert is not
    zigbee_spinel_attach(&client);
    reattach_answering();
    TEST_ASSERT_EQUAL(1, got.ready_count);
    TEST_ASSERT_EQUAL(3, count_sent(SPINEL_CMD_PROP_VALUE_INSERT, SPINEL_PROP_MAC_SRC_MATCH_SHORT_ADDRESSES));
}

TEST_CASE("attach with a timeout during the source-match replay: reports no ready", "[zigbee]")
{
    stored_three_shorts();
    rcp.silent_key = SPINEL_PROP_MAC_SRC_MATCH_SHORT_ADDRESSES;
    rcp.silent_cmd = SPINEL_CMD_PROP_VALUE_INSERT;
    zigbee_spinel_attach(&client);
    TEST_ASSERT_EQUAL(0, got.ready_count);
}

TEST_CASE("attach with a timeout during the source-match replay: requests one reset and sends nothing more",
          "[zigbee]")
{
    stored_three_shorts();
    rcp.silent_key = SPINEL_PROP_MAC_SRC_MATCH_SHORT_ADDRESSES;
    rcp.silent_cmd = SPINEL_CMD_PROP_VALUE_INSERT;
    zigbee_spinel_attach(&client);
    TEST_ASSERT_EQUAL(1, rcp.resets_requested);
    assert_sent(rcp.sent_count - 1, SPINEL_CMD_PROP_VALUE_INSERT, SPINEL_PROP_MAC_SRC_MATCH_SHORT_ADDRESSES);
    TEST_ASSERT_EQUAL(1, count_sent(SPINEL_CMD_PROP_VALUE_INSERT, SPINEL_PROP_MAC_SRC_MATCH_SHORT_ADDRESSES));
}

TEST_CASE("attach with a timeout clearing the tables in the replay: keeps the stored table, one reset", "[zigbee]")
{
    stored_three_shorts();
    rcp.silent_key = SPINEL_PROP_MAC_SRC_MATCH_SHORT_ADDRESSES;
    rcp.silent_cmd = SPINEL_CMD_PROP_VALUE_SET;
    zigbee_spinel_attach(&client);
    TEST_ASSERT_EQUAL(0, got.ready_count);
    TEST_ASSERT_EQUAL(1, rcp.resets_requested);
    assert_sent(rcp.sent_count - 1, SPINEL_CMD_PROP_VALUE_SET, SPINEL_PROP_MAC_SRC_MATCH_SHORT_ADDRESSES);
    reattach_answering();
    TEST_ASSERT_EQUAL(3, count_sent(SPINEL_CMD_PROP_VALUE_INSERT, SPINEL_PROP_MAC_SRC_MATCH_SHORT_ADDRESSES));
}

TEST_CASE("attach with a timeout during the configuration replay: reports no ready and requests one reset",
          "[zigbee]")
{
    attached(FRAMES_C6_RADIO_CAPS);
    zigbee_spinel_configure(&client, &CONFIG);
    zigbee_spinel_src_match_add_short(&client, 0x0001);
    rcp.silent_key = SPINEL_PROP_MAC_15_4_PANID;
    rcp.sent_count = 0;
    zigbee_spinel_attach(&client);
    TEST_ASSERT_EQUAL(0, got.ready_count);
    TEST_ASSERT_EQUAL(1, rcp.resets_requested);
    assert_sent(rcp.sent_count - 1, SPINEL_CMD_PROP_VALUE_SET, SPINEL_PROP_MAC_15_4_PANID);
}

TEST_CASE("src_match_clear with a timeout: stops after the first table and requests one reset", "[zigbee]")
{
    attached(FRAMES_C6_RADIO_CAPS);
    rcp.silent_key = SPINEL_PROP_MAC_SRC_MATCH_SHORT_ADDRESSES;
    zigbee_spinel_src_match_clear(&client);
    TEST_ASSERT_EQUAL(1, rcp.sent_count);
    TEST_ASSERT_EQUAL(1, rcp.resets_requested);
}

TEST_CASE("abort during an attach step: reports neither ready nor incompatible", "[zigbee]")
{
    setup(FRAMES_C6_RADIO_CAPS);
    rcp.abort_key = SPINEL_PROP_RADIO_CAPS;
    zigbee_spinel_attach(&client);
    TEST_ASSERT_EQUAL(0, got.ready_count);
}

TEST_CASE("abort during an attach step: requests no reset and sends nothing more", "[zigbee]")
{
    setup(FRAMES_C6_RADIO_CAPS);
    rcp.abort_key = SPINEL_PROP_RADIO_CAPS;
    zigbee_spinel_attach(&client);
    TEST_ASSERT_EQUAL(0, rcp.resets_requested);
    TEST_ASSERT_EQUAL(5, rcp.sent_count);
}

TEST_CASE("abort while waiting for the reset status: reports nothing and requests no reset", "[zigbee]")
{
    setup(FRAMES_C6_RADIO_CAPS);
    rcp.reset_silent = true;
    rcp.abort_pending = true;
    zigbee_spinel_attach(&client);
    TEST_ASSERT_EQUAL(0, got.ready_count);
    TEST_ASSERT_EQUAL(0, rcp.resets_requested);
    TEST_ASSERT_EQUAL(1, rcp.sent_count);
}

TEST_CASE("abort during a configure wait: returns nonzero", "[zigbee]")
{
    attached(FRAMES_C6_RADIO_CAPS);
    rcp.abort_key = SPINEL_PROP_PHY_TX_POWER;
    TEST_ASSERT_EQUAL(ZIGBEE_SPINEL_ERR_ABORTED, zigbee_spinel_configure(&client, &CONFIG));
    TEST_ASSERT_EQUAL(0, rcp.resets_requested);
}

TEST_CASE("abort during a configure wait: keeps the stored configuration", "[zigbee]")
{
    uint16_t pan = 0;

    attached(FRAMES_C6_RADIO_CAPS);
    rcp.abort_key = SPINEL_PROP_PHY_TX_POWER;
    zigbee_spinel_configure(&client, &CONFIG);
    reattach_answering();
    assert_sent(10, SPINEL_CMD_PROP_VALUE_SET, SPINEL_PROP_MAC_15_4_PANID);
    spinel_datatype_unpack(sent(10).data, sent(10).len, "S", &pan);
    TEST_ASSERT_EQUAL_HEX16(0x1a62, pan);
}

TEST_CASE("abort with no request waiting: changes nothing", "[zigbee]")
{
    attached(FRAMES_C6_RADIO_CAPS);
    zigbee_spinel_abort_wait(&client);
    TEST_ASSERT_EQUAL(0, zigbee_spinel_set_channel(&client, 20));
}

TEST_CASE("request timeout: returns ZIGBEE_SPINEL_ERR_TIMEOUT, distinct from an abort", "[zigbee]")
{
    attached(FRAMES_C6_RADIO_CAPS);
    rcp.silent_key = SPINEL_PROP_PHY_CHAN;
    TEST_ASSERT_EQUAL(ZIGBEE_SPINEL_ERR_TIMEOUT, zigbee_spinel_set_channel(&client, 20));
}

/* ---- radio resets and loss (Task 3) ---- */

static bool is_cmd_reset(size_t i)
{
    static const uint8_t reset[] = {0x80, SPINEL_CMD_RESET, SPINEL_RESET_STACK};

    TEST_ASSERT_TRUE_MESSAGE(i < rcp.sent_count, "no such request");
    return rcp.sent[i].len == sizeof(reset) && memcmp(rcp.sent[i].buf, reset, sizeof(reset)) == 0;
}

static size_t count_cmd_resets(void)
{
    size_t n = 0;

    for (size_t i = 0; i < rcp.sent_count; i++) {
        n += is_cmd_reset(i);
    }
    return n;
}

/* The index of the first request of `cmd`/`key` at or after `from`; fails the test when there is none. */
static size_t index_of(unsigned cmd, unsigned key, size_t from)
{
    for (size_t i = from; i < rcp.sent_count; i++) {
        if (!is_cmd_reset(i) && sent(i).cmd == cmd && sent(i).key == key) {
            return i;
        }
    }
    TEST_FAIL_MESSAGE("request not sent");
    return 0;
}

static void forget(void)
{
    rcp.sent_count = 0;
    rcp.resets_requested = 0;
    memset(&got, 0, sizeof(got));
}

static void transmitting(void)
{
    TEST_ASSERT_EQUAL(0, zigbee_spinel_transmit(&client, PSDU, sizeof(PSDU), 15, true, 3, 4));
}

static void scanning(void)
{
    TEST_ASSERT_EQUAL(0, zigbee_spinel_energy_scan(&client, 17, 250));
}

/* Attached, with the configuration and one entry in each source-match table stored; then forgotten. */
static void stored_config_and_table(void)
{
    attached(FRAMES_C6_RADIO_CAPS);
    TEST_ASSERT_EQUAL(0, zigbee_spinel_configure(&client, &CONFIG));
    TEST_ASSERT_EQUAL(0, zigbee_spinel_src_match_add_short(&client, 0x0001));
    TEST_ASSERT_EQUAL(0, zigbee_spinel_src_match_add_ext(&client, EXT_A));
    forget();
}

static void assert_replayed_once(void)
{
    TEST_ASSERT_EQUAL(1, count_sent(SPINEL_CMD_PROP_VALUE_SET, SPINEL_PROP_MAC_15_4_LADDR));
    TEST_ASSERT_EQUAL(1, count_sent(SPINEL_CMD_PROP_VALUE_INSERT, SPINEL_PROP_MAC_SRC_MATCH_SHORT_ADDRESSES));
    TEST_ASSERT_EQUAL(1, count_sent(SPINEL_CMD_PROP_VALUE_INSERT, SPINEL_PROP_MAC_SRC_MATCH_EXTENDED_ADDRESSES));
}

static void no_radio(void)
{
    attached(FRAMES_C6_RADIO_CAPS);
    zigbee_spinel_on_gone(&client);
    forget();
}

/* -- GONE -- */

TEST_CASE("gone: a transmit in flight fails", "[zigbee]")
{
    attached(FRAMES_C6_RADIO_CAPS);
    transmitting();
    zigbee_spinel_on_gone(&client);
    TEST_ASSERT_EQUAL(1, got.tx_count);
    TEST_ASSERT_EQUAL(ZIGBEE_TX_FAILED, got.tx);
}

TEST_CASE("gone: an energy scan in progress fails", "[zigbee]")
{
    attached(FRAMES_C6_RADIO_CAPS);
    scanning();
    zigbee_spinel_on_gone(&client);
    TEST_ASSERT_EQUAL(1, got.scan_count);
    TEST_ASSERT_EQUAL(ZIGBEE_SPINEL_SCAN_FAILED_RSSI, got.scan_rssi);
}

TEST_CASE("gone with a transmit in flight and an energy scan in progress: fails both at once", "[zigbee]")
{
    attached(FRAMES_C6_RADIO_CAPS);
    transmitting();
    scanning();
    int64_t start = rcp.now;
    zigbee_spinel_on_gone(&client);
    TEST_ASSERT_EQUAL(1, got.tx_count);
    TEST_ASSERT_EQUAL(1, got.scan_count);
    TEST_ASSERT_EQUAL_INT64(start, rcp.now);
}

TEST_CASE("gone: enters no radio", "[zigbee]")
{
    attached(FRAMES_C6_RADIO_CAPS);
    TEST_ASSERT_TRUE(zigbee_spinel_has_radio(&client));
    zigbee_spinel_on_gone(&client);
    TEST_ASSERT_FALSE(zigbee_spinel_has_radio(&client));
}

TEST_CASE("gone: sends nothing, requests no reset and counts no reset", "[zigbee]")
{
    attached(FRAMES_C6_RADIO_CAPS);
    transmitting();
    forget();
    zigbee_spinel_on_gone(&client);
    TEST_ASSERT_EQUAL(0, rcp.sent_count);
    TEST_ASSERT_EQUAL(0, rcp.resets_requested);
    TEST_ASSERT_EQUAL_UINT32(0, zigbee_spinel_resets(&client));
}

TEST_CASE("gone while a property request waits: the request returns ZIGBEE_SPINEL_ERR_ABORTED at once", "[zigbee]")
{
    attached(FRAMES_C6_RADIO_CAPS);
    rcp.event_key = SPINEL_PROP_PHY_CHAN;
    rcp.event = EVENT_GONE;
    int64_t start = rcp.now;
    TEST_ASSERT_EQUAL(ZIGBEE_SPINEL_ERR_ABORTED, zigbee_spinel_set_channel(&client, 20));
    TEST_ASSERT_TRUE(rcp.now - start < ZIGBEE_SPINEL_REQUEST_TIMEOUT_US);
    TEST_ASSERT_EQUAL(0, rcp.resets_requested);
    TEST_ASSERT_FALSE(zigbee_spinel_has_radio(&client));
}

TEST_CASE("gone during a configure: the call sends nothing more", "[zigbee]")
{
    attached(FRAMES_C6_RADIO_CAPS);
    rcp.event_key = SPINEL_PROP_PHY_TX_POWER;
    rcp.event = EVENT_GONE;
    TEST_ASSERT_EQUAL(ZIGBEE_SPINEL_ERR_ABORTED, zigbee_spinel_configure(&client, &CONFIG));
    TEST_ASSERT_EQUAL(2, rcp.sent_count); // PHY_CHAN, PHY_TX_POWER
}

TEST_CASE("gone during attach: reports no ready and sends nothing more", "[zigbee]")
{
    setup(FRAMES_C6_RADIO_CAPS);
    rcp.event_key = SPINEL_PROP_RADIO_CAPS;
    rcp.event = EVENT_GONE;
    zigbee_spinel_attach(&client);
    TEST_ASSERT_EQUAL(0, got.ready_count);
    TEST_ASSERT_EQUAL(5, rcp.sent_count);
    TEST_ASSERT_EQUAL(0, rcp.resets_requested);
    TEST_ASSERT_FALSE(zigbee_spinel_has_radio(&client));
}

TEST_CASE("gone while an energy scan starts: the call returns nonzero and no scan result is reported", "[zigbee]")
{
    attached(FRAMES_C6_RADIO_CAPS);
    rcp.event_key = SPINEL_PROP_MAC_SCAN_STATE;
    rcp.event = EVENT_GONE;
    TEST_ASSERT_NOT_EQUAL(0, zigbee_spinel_energy_scan(&client, 17, 250));
    TEST_ASSERT_EQUAL(0, got.scan_count);
}

TEST_CASE("gone in the wait that delivers a reply: the call sends nothing more", "[zigbee]")
{
    attached(FRAMES_C6_RADIO_CAPS);
    rcp.event_reply_key = SPINEL_PROP_PHY_TX_POWER;
    rcp.event = EVENT_GONE;
    TEST_ASSERT_EQUAL(ZIGBEE_SPINEL_ERR_ABORTED, zigbee_spinel_configure(&client, &CONFIG));
    TEST_ASSERT_EQUAL(2, rcp.sent_count); // PHY_CHAN, PHY_TX_POWER (answered)
}

TEST_CASE("gone in the wait that delivers the scan's last reply: the call returns nonzero, no scan result",
          "[zigbee]")
{
    attached(FRAMES_C6_RADIO_CAPS);
    rcp.event_reply_key = SPINEL_PROP_MAC_SCAN_STATE;
    rcp.event = EVENT_GONE;
    TEST_ASSERT_EQUAL(ZIGBEE_SPINEL_ERR_ABORTED, zigbee_spinel_energy_scan(&client, 17, 250));
    TEST_ASSERT_EQUAL(0, got.scan_count);
    TEST_ASSERT_EQUAL(ZIGBEE_SPINEL_ERR_LOCAL, zigbee_spinel_energy_scan(&client, 17, 250)); // no radio, not "busy"
}

/* -- with no radio -- */

TEST_CASE("has_radio: false before the first attach, true after it", "[zigbee]")
{
    setup(FRAMES_C6_RADIO_CAPS);
    TEST_ASSERT_FALSE(zigbee_spinel_has_radio(&client));
    zigbee_spinel_attach(&client);
    TEST_ASSERT_TRUE(zigbee_spinel_has_radio(&client));
}

TEST_CASE("configure with no radio: stores the configuration, sends nothing, returns 0", "[zigbee]")
{
    uint16_t pan = 0;

    no_radio();
    TEST_ASSERT_EQUAL(0, zigbee_spinel_configure(&client, &CONFIG));
    TEST_ASSERT_EQUAL(0, rcp.sent_count);
    zigbee_spinel_attach(&client);
    size_t i = index_of(SPINEL_CMD_PROP_VALUE_SET, SPINEL_PROP_MAC_15_4_PANID, 0);
    spinel_datatype_unpack(sent(i).data, sent(i).len, "S", &pan);
    TEST_ASSERT_EQUAL_HEX16(0x1a62, pan);
}

TEST_CASE("set_channel and set_promiscuous with no radio: stored, nothing sent, return 0", "[zigbee]")
{
    uint8_t channel = 0, mode = 0;

    attached(FRAMES_C6_RADIO_CAPS);
    zigbee_spinel_configure(&client, &CONFIG);
    zigbee_spinel_on_gone(&client);
    forget();
    TEST_ASSERT_EQUAL(0, zigbee_spinel_set_channel(&client, 21));
    TEST_ASSERT_EQUAL(0, zigbee_spinel_set_promiscuous(&client, true));
    TEST_ASSERT_EQUAL(0, rcp.sent_count);
    zigbee_spinel_attach(&client);
    size_t i = index_of(SPINEL_CMD_PROP_VALUE_SET, SPINEL_PROP_PHY_CHAN, 0);
    spinel_datatype_unpack(sent(i).data, sent(i).len, "C", &channel);
    TEST_ASSERT_EQUAL(21, channel);
    i = index_of(SPINEL_CMD_PROP_VALUE_SET, SPINEL_PROP_MAC_PROMISCUOUS_MODE, 0);
    spinel_datatype_unpack(sent(i).data, sent(i).len, "C", &mode);
    TEST_ASSERT_EQUAL(SPINEL_MAC_PROMISCUOUS_MODE_NETWORK, mode);
}

TEST_CASE("src_match adds with no radio: stored, nothing sent, return 0", "[zigbee]")
{
    attached(FRAMES_C6_RADIO_CAPS);
    zigbee_spinel_src_match_add_short(&client, 0x0001);
    zigbee_spinel_on_gone(&client);
    forget();
    TEST_ASSERT_EQUAL(0, zigbee_spinel_src_match_add_short(&client, 0x0002));
    TEST_ASSERT_EQUAL(0, zigbee_spinel_src_match_add_ext(&client, EXT_A));
    TEST_ASSERT_EQUAL(0, rcp.sent_count);
    zigbee_spinel_attach(&client);
    TEST_ASSERT_EQUAL(2, count_sent(SPINEL_CMD_PROP_VALUE_INSERT, SPINEL_PROP_MAC_SRC_MATCH_SHORT_ADDRESSES));
    TEST_ASSERT_EQUAL(1, count_sent(SPINEL_CMD_PROP_VALUE_INSERT, SPINEL_PROP_MAC_SRC_MATCH_EXTENDED_ADDRESSES));
}

TEST_CASE("src_match_clear with no radio: empties the stored copy and sends nothing", "[zigbee]")
{
    attached(FRAMES_C6_RADIO_CAPS);
    zigbee_spinel_src_match_add_short(&client, 0x0001);
    zigbee_spinel_on_gone(&client);
    forget();
    zigbee_spinel_src_match_clear(&client);
    TEST_ASSERT_EQUAL(0, rcp.sent_count);
    zigbee_spinel_attach(&client);
    TEST_ASSERT_EQUAL(0, count_sent(SPINEL_CMD_PROP_VALUE_INSERT, SPINEL_PROP_MAC_SRC_MATCH_SHORT_ADDRESSES));
}

TEST_CASE("transmit with no radio: returns nonzero at once", "[zigbee]")
{
    no_radio();
    TEST_ASSERT_EQUAL(ZIGBEE_SPINEL_ERR_LOCAL, zigbee_spinel_transmit(&client, PSDU, sizeof(PSDU), 15, true, 3, 4));
    TEST_ASSERT_EQUAL(0, rcp.sent_count);
    TEST_ASSERT_EQUAL(0, got.tx_count);
}

TEST_CASE("energy_scan with no radio: returns nonzero at once", "[zigbee]")
{
    no_radio();
    int64_t start = rcp.now;
    TEST_ASSERT_EQUAL(ZIGBEE_SPINEL_ERR_LOCAL, zigbee_spinel_energy_scan(&client, 17, 250));
    TEST_ASSERT_EQUAL(0, rcp.sent_count);
    TEST_ASSERT_EQUAL(0, got.scan_count);
    TEST_ASSERT_EQUAL_INT64(start, rcp.now);
}

TEST_CASE("reset with no radio: ignored", "[zigbee]")
{
    no_radio();
    zigbee_spinel_on_reset(&client);
    TEST_ASSERT_EQUAL(0, rcp.sent_count);
    TEST_ASSERT_EQUAL(0, got.ready_count);
    TEST_ASSERT_EQUAL_UINT32(0, zigbee_spinel_resets(&client));
}

TEST_CASE("unsolicited reset status with no radio: ignored", "[zigbee]")
{
    uint8_t buf[32];

    no_radio();
    deliver(buf, frames_reset_status(buf, sizeof(buf), 0, SPINEL_STATUS_RESET_SOFTWARE));
    TEST_ASSERT_EQUAL(0, rcp.sent_count);
    TEST_ASSERT_EQUAL_UINT32(0, zigbee_spinel_resets(&client));
}

/* -- attach after GONE -- */

TEST_CASE("attach after gone: runs the attach sequence and replays the stored configuration and table", "[zigbee]")
{
    stored_config_and_table();
    zigbee_spinel_on_gone(&client);
    zigbee_spinel_attach(&client);
    TEST_ASSERT_TRUE(is_cmd_reset(0));
    assert_sent(1, SPINEL_CMD_PROP_VALUE_GET, SPINEL_PROP_PROTOCOL_VERSION);
    assert_replayed_once();
}

TEST_CASE("attach after gone: reports ready and has a radio again", "[zigbee]")
{
    stored_config_and_table();
    zigbee_spinel_on_gone(&client);
    zigbee_spinel_attach(&client);
    TEST_ASSERT_EQUAL(1, got.ready_count);
    TEST_ASSERT_TRUE(got.compatible);
    TEST_ASSERT_TRUE(zigbee_spinel_has_radio(&client));
}

TEST_CASE("reattach: a transmit after replay is accepted", "[zigbee]")
{
    attached(FRAMES_C6_RADIO_CAPS);
    transmitting();
    zigbee_spinel_on_gone(&client);
    zigbee_spinel_attach(&client);
    forget();
    TEST_ASSERT_EQUAL(0, zigbee_spinel_transmit(&client, PSDU, sizeof(PSDU), 15, true, 3, 4));
    TEST_ASSERT_EQUAL(1, rcp.sent_count);
    assert_sent(0, SPINEL_CMD_PROP_VALUE_SET, SPINEL_PROP_STREAM_RAW);
}

TEST_CASE("reattach: an energy scan after replay is accepted", "[zigbee]")
{
    attached(FRAMES_C6_RADIO_CAPS);
    scanning();
    zigbee_spinel_on_gone(&client);
    zigbee_spinel_attach(&client);
    TEST_ASSERT_EQUAL(0, zigbee_spinel_energy_scan(&client, 17, 250));
}

TEST_CASE("attach with a transmit that timed out: the transmit fails once and the next one is accepted", "[zigbee]")
{
    attached(FRAMES_C6_RADIO_CAPS);
    transmitting();
    rcp.now += ZIGBEE_SPINEL_TX_TIMEOUT_US;
    zigbee_spinel_on_tick(&client);
    TEST_ASSERT_EQUAL(1, rcp.resets_requested);
    TEST_ASSERT_EQUAL(0, got.tx_count);
    zigbee_spinel_attach(&client);
    TEST_ASSERT_EQUAL(1, got.tx_count);
    TEST_ASSERT_EQUAL(ZIGBEE_TX_FAILED, got.tx);
    TEST_ASSERT_EQUAL(0, zigbee_spinel_transmit(&client, PSDU, sizeof(PSDU), 15, true, 3, 4));
}

TEST_CASE("attach with an energy scan that timed out: the scan fails once and the next one is accepted", "[zigbee]")
{
    attached(FRAMES_C6_RADIO_CAPS);
    scanning();
    rcp.now += 250 * 1000LL + ZIGBEE_SPINEL_SCAN_GRACE_US;
    zigbee_spinel_on_tick(&client);
    TEST_ASSERT_EQUAL(1, rcp.resets_requested);
    zigbee_spinel_attach(&client);
    TEST_ASSERT_EQUAL(1, got.scan_count);
    TEST_ASSERT_EQUAL(ZIGBEE_SPINEL_SCAN_FAILED_RSSI, got.scan_rssi);
    TEST_ASSERT_EQUAL(0, zigbee_spinel_energy_scan(&client, 17, 250));
}

TEST_CASE("attach arriving inside a wait: the waiting call returns ZIGBEE_SPINEL_ERR_ABORTED, then the attach runs",
          "[zigbee]")
{
    attached(FRAMES_C6_RADIO_CAPS);
    rcp.event_key = SPINEL_PROP_PHY_CHAN;
    rcp.event = EVENT_GONE_ATTACH;
    TEST_ASSERT_EQUAL(ZIGBEE_SPINEL_ERR_ABORTED, zigbee_spinel_set_channel(&client, 20));
    TEST_ASSERT_TRUE(is_cmd_reset(1)); // right after the aborted PHY_CHAN
    TEST_ASSERT_EQUAL(1, count_cmd_resets());
    TEST_ASSERT_EQUAL(1, got.ready_count);
    TEST_ASSERT_TRUE(zigbee_spinel_has_radio(&client));
}

/* -- RESET -- */

TEST_CASE("reset: runs the attach sequence and replays the stored configuration and table", "[zigbee]")
{
    stored_config_and_table();
    zigbee_spinel_on_reset(&client);
    TEST_ASSERT_TRUE(is_cmd_reset(0));
    assert_sent(1, SPINEL_CMD_PROP_VALUE_GET, SPINEL_PROP_PROTOCOL_VERSION);
    assert_replayed_once();
    TEST_ASSERT_EQUAL(1, got.ready_count);
    TEST_ASSERT_TRUE(got.compatible);
}

TEST_CASE("reset: requests no channel reset and counts one reset", "[zigbee]")
{
    attached(FRAMES_C6_RADIO_CAPS);
    TEST_ASSERT_EQUAL_UINT32(0, zigbee_spinel_resets(&client));
    zigbee_spinel_on_reset(&client);
    TEST_ASSERT_EQUAL(0, rcp.resets_requested);
    TEST_ASSERT_EQUAL_UINT32(1, zigbee_spinel_resets(&client));
}

TEST_CASE("reset: a transmit in flight fails", "[zigbee]")
{
    attached(FRAMES_C6_RADIO_CAPS);
    transmitting();
    zigbee_spinel_on_reset(&client);
    TEST_ASSERT_EQUAL(1, got.tx_count);
    TEST_ASSERT_EQUAL(ZIGBEE_TX_FAILED, got.tx);
}

TEST_CASE("reset: an energy scan in progress fails", "[zigbee]")
{
    attached(FRAMES_C6_RADIO_CAPS);
    scanning();
    zigbee_spinel_on_reset(&client);
    TEST_ASSERT_EQUAL(1, got.scan_count);
    TEST_ASSERT_EQUAL(ZIGBEE_SPINEL_SCAN_FAILED_RSSI, got.scan_rssi);
}

TEST_CASE("reset: a late answer to the failed transmit is not reported again", "[zigbee]")
{
    uint8_t buf[96];

    attached(FRAMES_C6_RADIO_CAPS);
    transmitting();
    uint8_t tid = sent_tid(0);
    zigbee_spinel_on_reset(&client);
    deliver(buf, frames_tx_done(buf, sizeof(buf), tid, SPINEL_STATUS_OK));
    TEST_ASSERT_EQUAL(1, got.tx_count);
    TEST_ASSERT_EQUAL(ZIGBEE_TX_FAILED, got.tx);
}

TEST_CASE("reset: a transmit after replay is accepted", "[zigbee]")
{
    attached(FRAMES_C6_RADIO_CAPS);
    transmitting();
    zigbee_spinel_on_reset(&client);
    TEST_ASSERT_EQUAL(0, zigbee_spinel_transmit(&client, PSDU, sizeof(PSDU), 15, true, 3, 4));
}

TEST_CASE("reset after a transmit timed out: the transmit fails once", "[zigbee]")
{
    attached(FRAMES_C6_RADIO_CAPS);
    transmitting();
    rcp.now += ZIGBEE_SPINEL_TX_TIMEOUT_US;
    zigbee_spinel_on_tick(&client);
    zigbee_spinel_on_reset(&client);
    zigbee_spinel_on_tick(&client);
    TEST_ASSERT_EQUAL(1, got.tx_count);
    TEST_ASSERT_EQUAL(ZIGBEE_TX_FAILED, got.tx);
    TEST_ASSERT_EQUAL(1, rcp.resets_requested);
}

TEST_CASE("reset on an RCP without rx-on-when-idle: a pending re-arm is dropped", "[zigbee]")
{
    attached(FRAMES_CC2652P_RADIO_CAPS);
    rcp.silent_key = SPINEL_PROP_MAC_RAW_STREAM_ENABLED;
    rcp.silent_cmd = SPINEL_CMD_PROP_VALUE_SET;
    transmitting();
    answer_tx(SPINEL_STATUS_OK); // the re-arm goes out and gets no reply
    rcp.silent_key = UINT32_MAX;
    zigbee_spinel_on_reset(&client);
    rcp.now += ZIGBEE_SPINEL_REQUEST_TIMEOUT_US;
    zigbee_spinel_on_tick(&client);
    TEST_ASSERT_EQUAL(0, rcp.resets_requested);
}

TEST_CASE("reset after a request timed out in the replay: the attach runs again and reports ready", "[zigbee]")
{
    stored_three_shorts();
    rcp.silent_key = SPINEL_PROP_MAC_SRC_MATCH_SHORT_ADDRESSES;
    rcp.silent_cmd = SPINEL_CMD_PROP_VALUE_INSERT;
    zigbee_spinel_attach(&client);
    TEST_ASSERT_EQUAL(0, got.ready_count);
    TEST_ASSERT_EQUAL(1, rcp.resets_requested);
    rcp.silent_key = UINT32_MAX;
    forget();
    zigbee_spinel_on_reset(&client); // the mux's answer to the requested reset
    TEST_ASSERT_EQUAL(1, got.ready_count);
    TEST_ASSERT_EQUAL(3, count_sent(SPINEL_CMD_PROP_VALUE_INSERT, SPINEL_PROP_MAC_SRC_MATCH_SHORT_ADDRESSES));
}

TEST_CASE("reset while a property request waits: the request returns ZIGBEE_SPINEL_ERR_ABORTED at once", "[zigbee]")
{
    attached(FRAMES_C6_RADIO_CAPS);
    rcp.event_key = SPINEL_PROP_PHY_CHAN;
    rcp.event = EVENT_RESET;
    int64_t start = rcp.now;
    TEST_ASSERT_EQUAL(ZIGBEE_SPINEL_ERR_ABORTED, zigbee_spinel_set_channel(&client, 20));
    TEST_ASSERT_TRUE(rcp.now - start < ZIGBEE_SPINEL_REQUEST_TIMEOUT_US);
    TEST_ASSERT_EQUAL(0, rcp.resets_requested);
}

TEST_CASE("reset while a property request waits: the attach runs once the call unwinds and replays", "[zigbee]")
{
    stored_config_and_table();
    rcp.event_key = SPINEL_PROP_PHY_CHAN;
    rcp.event = EVENT_RESET;
    zigbee_spinel_set_channel(&client, 20);
    assert_sent(0, SPINEL_CMD_PROP_VALUE_SET, SPINEL_PROP_PHY_CHAN);
    TEST_ASSERT_TRUE(is_cmd_reset(1));
    TEST_ASSERT_EQUAL(1, count_cmd_resets());
    assert_replayed_once();
    TEST_ASSERT_EQUAL(1, got.ready_count);
}

TEST_CASE("reset during a configure: the call sends nothing more before the attach", "[zigbee]")
{
    attached(FRAMES_C6_RADIO_CAPS);
    rcp.event_key = SPINEL_PROP_PHY_TX_POWER;
    rcp.event = EVENT_RESET;
    TEST_ASSERT_EQUAL(ZIGBEE_SPINEL_ERR_ABORTED, zigbee_spinel_configure(&client, &CONFIG));
    assert_sent(0, SPINEL_CMD_PROP_VALUE_SET, SPINEL_PROP_PHY_CHAN);
    assert_sent(1, SPINEL_CMD_PROP_VALUE_SET, SPINEL_PROP_PHY_TX_POWER);
    TEST_ASSERT_TRUE(is_cmd_reset(2));
    TEST_ASSERT_EQUAL(1, count_sent(SPINEL_CMD_PROP_VALUE_SET, SPINEL_PROP_MAC_15_4_PANID)); // the replay's
    TEST_ASSERT_EQUAL(1, got.ready_count);
}

TEST_CASE("reset in the wait that delivers a reply: the call sends nothing more before the attach", "[zigbee]")
{
    attached(FRAMES_C6_RADIO_CAPS);
    rcp.event_reply_key = SPINEL_PROP_PHY_TX_POWER;
    rcp.event = EVENT_RESET;
    TEST_ASSERT_EQUAL(ZIGBEE_SPINEL_ERR_ABORTED, zigbee_spinel_configure(&client, &CONFIG));
    assert_sent(1, SPINEL_CMD_PROP_VALUE_SET, SPINEL_PROP_PHY_TX_POWER);
    TEST_ASSERT_TRUE(is_cmd_reset(2));
    TEST_ASSERT_EQUAL(1, count_sent(SPINEL_CMD_PROP_VALUE_SET, SPINEL_PROP_MAC_15_4_PANID)); // the replay's
    TEST_ASSERT_EQUAL(1, got.ready_count);
}

TEST_CASE("reset in the wait that delivers the scan's last reply: the call returns nonzero, the next scan runs",
          "[zigbee]")
{
    attached(FRAMES_C6_RADIO_CAPS);
    rcp.event_reply_key = SPINEL_PROP_MAC_SCAN_STATE;
    rcp.event = EVENT_RESET;
    TEST_ASSERT_EQUAL(ZIGBEE_SPINEL_ERR_ABORTED, zigbee_spinel_energy_scan(&client, 17, 250));
    TEST_ASSERT_EQUAL(0, got.scan_count);
    TEST_ASSERT_EQUAL(1, got.ready_count);
    TEST_ASSERT_EQUAL(0, zigbee_spinel_energy_scan(&client, 17, 250));
}

TEST_CASE("reset during attach: the sequence restarts and replays once", "[zigbee]")
{
    stored_config_and_table();
    rcp.event_key = SPINEL_PROP_RADIO_CAPS;
    rcp.event = EVENT_RESET;
    int64_t start = rcp.now;
    zigbee_spinel_on_reset(&client);
    TEST_ASSERT_EQUAL(2, count_cmd_resets());
    TEST_ASSERT_TRUE(is_cmd_reset(0));
    assert_sent(4, SPINEL_CMD_PROP_VALUE_GET, SPINEL_PROP_RADIO_CAPS);
    TEST_ASSERT_TRUE(is_cmd_reset(5)); // straight after the interrupted step
    assert_replayed_once();
    TEST_ASSERT_EQUAL(1, got.ready_count);
    TEST_ASSERT_TRUE(got.compatible);
    TEST_ASSERT_EQUAL_UINT32(2, zigbee_spinel_resets(&client));
    TEST_ASSERT_EQUAL(0, rcp.resets_requested);
    TEST_ASSERT_TRUE(rcp.now - start < ZIGBEE_SPINEL_REQUEST_TIMEOUT_US); // no request waited out its timeout
}

TEST_CASE("reset during the attach's replay: the sequence restarts and the replay completes once", "[zigbee]")
{
    stored_config_and_table();
    rcp.event_key = SPINEL_PROP_MAC_15_4_PANID;
    rcp.event = EVENT_RESET;
    zigbee_spinel_on_reset(&client);
    TEST_ASSERT_EQUAL(2, count_cmd_resets());
    TEST_ASSERT_EQUAL(2, count_sent(SPINEL_CMD_PROP_VALUE_SET, SPINEL_PROP_MAC_15_4_PANID)); // cut short, then whole
    assert_replayed_once();
    TEST_ASSERT_EQUAL(1, got.ready_count);
}

TEST_CASE("reset while waiting for the reset status: the sequence restarts from CMD_RESET", "[zigbee]")
{
    attached(FRAMES_C6_RADIO_CAPS);
    rcp.event_key = KEY_CMD_RESET;
    rcp.event = EVENT_RESET;
    int64_t start = rcp.now;
    zigbee_spinel_on_reset(&client);
    TEST_ASSERT_TRUE(is_cmd_reset(0));
    TEST_ASSERT_TRUE(is_cmd_reset(1));
    TEST_ASSERT_EQUAL(2, count_cmd_resets());
    TEST_ASSERT_EQUAL(1, got.ready_count);
    TEST_ASSERT_TRUE(rcp.now - start < ZIGBEE_SPINEL_REQUEST_TIMEOUT_US);
}

/* -- the RCP's own reset (unsolicited LAST_STATUS in the reset range) -- */

TEST_CASE("unsolicited reset status: runs the attach sequence and replays, as a reset", "[zigbee]")
{
    uint8_t buf[32];

    stored_config_and_table();
    deliver(buf, frames_reset_status(buf, sizeof(buf), 0, SPINEL_STATUS_RESET_SOFTWARE));
    TEST_ASSERT_TRUE(is_cmd_reset(0));
    assert_replayed_once();
    TEST_ASSERT_EQUAL(1, got.ready_count);
    TEST_ASSERT_EQUAL_UINT32(1, zigbee_spinel_resets(&client));
}

TEST_CASE("unsolicited reset status: a transmit in flight fails", "[zigbee]")
{
    uint8_t buf[32];

    attached(FRAMES_C6_RADIO_CAPS);
    transmitting();
    deliver(buf, frames_reset_status(buf, sizeof(buf), 0, SPINEL_STATUS_RESET_CRASH));
    TEST_ASSERT_EQUAL(1, got.tx_count);
    TEST_ASSERT_EQUAL(ZIGBEE_TX_FAILED, got.tx);
}

TEST_CASE("unsolicited reset status on the broadcast IID: handled as a reset", "[zigbee]")
{
    uint8_t buf[32];

    attached(FRAMES_C6_RADIO_CAPS);
    deliver(buf, frames_reset_status(buf, sizeof(buf), 3, SPINEL_STATUS_RESET_POWER_ON));
    TEST_ASSERT_EQUAL_UINT32(1, zigbee_spinel_resets(&client));
    TEST_ASSERT_EQUAL(1, got.ready_count);
}

TEST_CASE("unsolicited reset status outside the reset range: ignored", "[zigbee]")
{
    uint8_t buf[32];

    attached(FRAMES_C6_RADIO_CAPS);
    deliver(buf, frames_reset_status(buf, sizeof(buf), 0, SPINEL_STATUS_RESET__END + 1));
    deliver(buf, frames_reset_status(buf, sizeof(buf), 0, SPINEL_STATUS_RESET__BEGIN - 1));
    TEST_ASSERT_EQUAL(0, rcp.sent_count);
    TEST_ASSERT_EQUAL_UINT32(0, zigbee_spinel_resets(&client));
}

TEST_CASE("unsolicited reset status while attaching: restarts the sequence from CMD_RESET and replays once",
          "[zigbee]")
{
    stored_config_and_table();
    rcp.reset_status_key = SPINEL_PROP_CAPS;
    int64_t start = rcp.now;
    zigbee_spinel_attach(&client);
    TEST_ASSERT_EQUAL(2, count_cmd_resets());
    assert_sent(3, SPINEL_CMD_PROP_VALUE_GET, SPINEL_PROP_CAPS);
    TEST_ASSERT_TRUE(is_cmd_reset(4));
    assert_replayed_once();
    TEST_ASSERT_EQUAL(1, got.ready_count);
    TEST_ASSERT_EQUAL_UINT32(1, zigbee_spinel_resets(&client));
    TEST_ASSERT_TRUE(rcp.now - start < ZIGBEE_SPINEL_REQUEST_TIMEOUT_US);
}

TEST_CASE("unsolicited reset status while a property request waits: the request returns "
          "ZIGBEE_SPINEL_ERR_ABORTED, then the attach runs", "[zigbee]")
{
    attached(FRAMES_C6_RADIO_CAPS);
    rcp.reset_status_key = SPINEL_PROP_PHY_CHAN;
    TEST_ASSERT_EQUAL(ZIGBEE_SPINEL_ERR_ABORTED, zigbee_spinel_set_channel(&client, 20));
    TEST_ASSERT_TRUE(is_cmd_reset(1));
    TEST_ASSERT_EQUAL(1, got.ready_count);
    TEST_ASSERT_EQUAL(0, rcp.resets_requested);
}

/* -- source-match adds a RESET, GONE or timeout cut short -- */

/* How many times the short address `address` was inserted. */
static size_t short_inserts(uint16_t address)
{
    size_t n = 0;

    for (size_t i = 0; i < rcp.sent_count; i++) {
        req_t r = sent(i);
        uint16_t a = 0;

        if (r.cmd == SPINEL_CMD_PROP_VALUE_INSERT && r.key == SPINEL_PROP_MAC_SRC_MATCH_SHORT_ADDRESSES &&
                spinel_datatype_unpack(r.data, r.len, "S", &a) > 0 && a == address) {
            n++;
        }
    }
    return n;
}

/* How many times EXT_A was inserted. */
static size_t ext_a_inserts(void)
{
    size_t n = 0;

    for (size_t i = 0; i < rcp.sent_count; i++) {
        req_t r = sent(i);

        if (r.cmd == SPINEL_CMD_PROP_VALUE_INSERT && r.key == SPINEL_PROP_MAC_SRC_MATCH_EXTENDED_ADDRESSES &&
                r.len == 8 && memcmp(r.data, EXT_A, 8) == 0) {
            n++;
        }
    }
    return n;
}

TEST_CASE("src_match_add_short aborted by a reset: returns nonzero and the attach replays the entry", "[zigbee]")
{
    attached(FRAMES_C6_RADIO_CAPS);
    rcp.event_key = SPINEL_PROP_MAC_SRC_MATCH_SHORT_ADDRESSES;
    rcp.event = EVENT_RESET;
    TEST_ASSERT_LESS_THAN(0, zigbee_spinel_src_match_add_short(&client, 0x1234));
    TEST_ASSERT_EQUAL(1, got.ready_count);
    TEST_ASSERT_EQUAL(2, short_inserts(0x1234)); // the aborted add, then the replay's
}

TEST_CASE("src_match_add_ext aborted by a reset: returns nonzero and the attach replays the entry", "[zigbee]")
{
    attached(FRAMES_C6_RADIO_CAPS);
    rcp.event_key = SPINEL_PROP_MAC_SRC_MATCH_EXTENDED_ADDRESSES;
    rcp.event = EVENT_RESET;
    TEST_ASSERT_LESS_THAN(0, zigbee_spinel_src_match_add_ext(&client, EXT_A));
    TEST_ASSERT_EQUAL(1, got.ready_count);
    TEST_ASSERT_EQUAL(2, ext_a_inserts());
}

TEST_CASE("src_match_add_short aborted by gone: returns nonzero and the next attach replays the entry", "[zigbee]")
{
    attached(FRAMES_C6_RADIO_CAPS);
    rcp.event_key = SPINEL_PROP_MAC_SRC_MATCH_SHORT_ADDRESSES;
    rcp.event = EVENT_GONE;
    TEST_ASSERT_LESS_THAN(0, zigbee_spinel_src_match_add_short(&client, 0x1234));
    forget();
    zigbee_spinel_attach(&client);
    TEST_ASSERT_EQUAL(1, got.ready_count);
    TEST_ASSERT_EQUAL(1, short_inserts(0x1234));
}

TEST_CASE("src_match_add_ext aborted by gone: returns nonzero and the next attach replays the entry", "[zigbee]")
{
    attached(FRAMES_C6_RADIO_CAPS);
    rcp.event_key = SPINEL_PROP_MAC_SRC_MATCH_EXTENDED_ADDRESSES;
    rcp.event = EVENT_GONE;
    TEST_ASSERT_LESS_THAN(0, zigbee_spinel_src_match_add_ext(&client, EXT_A));
    forget();
    zigbee_spinel_attach(&client);
    TEST_ASSERT_EQUAL(1, got.ready_count);
    TEST_ASSERT_EQUAL(1, ext_a_inserts());
}

TEST_CASE("src_match_add_short timed out: returns ZIGBEE_SPINEL_ERR_TIMEOUT and the reset's attach replays the entry",
          "[zigbee]")
{
    attached(FRAMES_C6_RADIO_CAPS);
    rcp.silent_key = SPINEL_PROP_MAC_SRC_MATCH_SHORT_ADDRESSES;
    rcp.silent_cmd = SPINEL_CMD_PROP_VALUE_INSERT;
    TEST_ASSERT_EQUAL(ZIGBEE_SPINEL_ERR_TIMEOUT, zigbee_spinel_src_match_add_short(&client, 0x1234));
    TEST_ASSERT_EQUAL(1, rcp.resets_requested);
    rcp.silent_key = UINT32_MAX;
    forget();
    zigbee_spinel_on_reset(&client); // the mux's answer to the requested reset
    TEST_ASSERT_EQUAL(1, got.ready_count);
    TEST_ASSERT_EQUAL(1, short_inserts(0x1234));
}

TEST_CASE("src_match_add_ext timed out: returns ZIGBEE_SPINEL_ERR_TIMEOUT and the reset's attach replays the entry",
          "[zigbee]")
{
    attached(FRAMES_C6_RADIO_CAPS);
    rcp.silent_key = SPINEL_PROP_MAC_SRC_MATCH_EXTENDED_ADDRESSES;
    rcp.silent_cmd = SPINEL_CMD_PROP_VALUE_INSERT;
    TEST_ASSERT_EQUAL(ZIGBEE_SPINEL_ERR_TIMEOUT, zigbee_spinel_src_match_add_ext(&client, EXT_A));
    rcp.silent_key = UINT32_MAX;
    forget();
    zigbee_spinel_on_reset(&client);
    TEST_ASSERT_EQUAL(1, got.ready_count);
    TEST_ASSERT_EQUAL(1, ext_a_inserts());
}

TEST_CASE("src_match add the RCP refuses: not stored, so the next attach does not replay it", "[zigbee]")
{
    attached(FRAMES_C6_RADIO_CAPS);
    rcp.short_limit = 1;
    rcp.ext_limit = 1;
    zigbee_spinel_src_match_clear(&client);
    TEST_ASSERT_EQUAL(0, zigbee_spinel_src_match_add_short(&client, 0x0001));
    TEST_ASSERT_GREATER_THAN(0, zigbee_spinel_src_match_add_short(&client, 0x1234));
    TEST_ASSERT_EQUAL(0, zigbee_spinel_src_match_add_ext(&client, EXT_B));
    TEST_ASSERT_GREATER_THAN(0, zigbee_spinel_src_match_add_ext(&client, EXT_A));
    forget();
    zigbee_spinel_attach(&client);
    TEST_ASSERT_EQUAL(0, short_inserts(0x1234));
    TEST_ASSERT_EQUAL(0, ext_a_inserts());
}

/* -- a deadline passes while the channel cannot be reset -- */

TEST_CASE("transmit timed out and the reset request refused: the transmit fails at once", "[zigbee]")
{
    attached(FRAMES_C6_RADIO_CAPS);
    transmitting();
    rcp.reset_result = 1;
    rcp.now += ZIGBEE_SPINEL_TX_TIMEOUT_US;
    zigbee_spinel_on_tick(&client);
    TEST_ASSERT_EQUAL(1, rcp.resets_requested);
    TEST_ASSERT_EQUAL(1, got.tx_count);
    TEST_ASSERT_EQUAL(ZIGBEE_TX_FAILED, got.tx);
}

TEST_CASE("transmit timed out and the reset request refused: the next transmit is sent", "[zigbee]")
{
    attached(FRAMES_C6_RADIO_CAPS);
    transmitting();
    rcp.reset_result = 1;
    rcp.now += ZIGBEE_SPINEL_TX_TIMEOUT_US;
    zigbee_spinel_on_tick(&client);
    TEST_ASSERT_EQUAL(0, zigbee_spinel_transmit(&client, PSDU, sizeof(PSDU), 15, true, 3, 4));
    TEST_ASSERT_EQUAL(2, rcp.sent_count);
}

TEST_CASE("transmit timed out and the reset request refused: a late answer to it is not reported", "[zigbee]")
{
    uint8_t buf[96];

    attached(FRAMES_C6_RADIO_CAPS);
    transmitting();
    uint8_t tid = sent_tid(0);
    rcp.reset_result = 1;
    rcp.now += ZIGBEE_SPINEL_TX_TIMEOUT_US;
    zigbee_spinel_on_tick(&client);
    deliver(buf, frames_tx_done(buf, sizeof(buf), tid, SPINEL_STATUS_OK));
    TEST_ASSERT_EQUAL(1, got.tx_count);
    TEST_ASSERT_EQUAL(ZIGBEE_TX_FAILED, got.tx);
}

TEST_CASE("transmit timed out and the reset request accepted: the transmit stays in flight until the reset",
          "[zigbee]")
{
    attached(FRAMES_C6_RADIO_CAPS);
    transmitting();
    rcp.now += ZIGBEE_SPINEL_TX_TIMEOUT_US;
    zigbee_spinel_on_tick(&client);
    TEST_ASSERT_EQUAL(0, got.tx_count);
    TEST_ASSERT_NOT_EQUAL(0, zigbee_spinel_transmit(&client, PSDU, sizeof(PSDU), 15, true, 3, 4));
}

TEST_CASE("energy scan timed out and the reset request refused: the scan fails at once and the next one is accepted",
          "[zigbee]")
{
    attached(FRAMES_C6_RADIO_CAPS);
    scanning();
    rcp.reset_result = 1;
    rcp.now += 250 * 1000LL + ZIGBEE_SPINEL_SCAN_GRACE_US;
    zigbee_spinel_on_tick(&client);
    TEST_ASSERT_EQUAL(1, got.scan_count);
    TEST_ASSERT_EQUAL(ZIGBEE_SPINEL_SCAN_FAILED_RSSI, got.scan_rssi);
    TEST_ASSERT_EQUAL(0, zigbee_spinel_energy_scan(&client, 17, 250));
}
