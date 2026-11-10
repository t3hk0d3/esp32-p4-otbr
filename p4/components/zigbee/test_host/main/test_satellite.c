#include <string.h>

#include "golden_zloe.h"
#include "unity.h"
#include "zigbee_satellite.h"

#define MAX_FRAMES 8
#define NOW_UNIX 1790000000

static struct {
    int64_t now;
    bool clock_valid;
    uint8_t frames[MAX_FRAMES][128];
    size_t lens[MAX_FRAMES];
    int count;
    int netinfo_requests;
    uint8_t epan[8];
    int announces;
    uint16_t announced_nwk;
} t;

static int64_t op_now(void *ctx)
{
    return t.now;
}

static bool op_clock(void *ctx, int64_t *out)
{
    *out = NOW_UNIX;
    return t.clock_valid;
}

static void op_frame(void *ctx, const uint8_t *frame, size_t len)
{
    TEST_ASSERT_LESS_THAN(MAX_FRAMES, t.count);
    memcpy(t.frames[t.count], frame, len);
    t.lens[t.count++] = len;
}

static bool op_netinfo(void *ctx)
{
    t.netinfo_requests++;
    return true;
}

static void op_epan(void *ctx, const uint8_t epan[8])
{
    memcpy(t.epan, epan, 8);
}

static void op_announce(void *ctx, const uint8_t eui64[8], const uint8_t epan[8], uint16_t nwk)
{
    t.announces++;
    t.announced_nwk = nwk;
}

static const zigbee_satellite_ops_t OPS = {op_now, op_clock, op_frame, op_netinfo, op_epan, op_announce};
static zigbee_satellite_t s;

static void begin(void)
{
    memset(&t, 0, sizeof(t));
    zigbee_satellite_init(&s, &OPS, NULL);
    zigbee_satellite_start(&s);
}

static void to_waiting_coordinator(void)
{
    begin();
    zigbee_satellite_on_frame(&s, GOLD_RSP_HW, sizeof(GOLD_RSP_HW));
    t.clock_valid = true;
    zigbee_satellite_on_tick(&s);
}

static void to_configuring(void)
{
    to_waiting_coordinator();
    zigbee_satellite_on_netinfo(&s, GOLD_NETINFO, sizeof(GOLD_NETINFO));
}

static void to_running(void)
{
    to_configuring();
    zigbee_satellite_on_frame(&s, GOLD_RSP_EMPTY_CONFIGURE, sizeof(GOLD_RSP_EMPTY_CONFIGURE));
    const uint8_t rsp[] = {0x16, 0x0d, 0x00, 0x00}; // StartNetwork, id 3, Ok

    zigbee_satellite_on_frame(&s, rsp, sizeof(rsp));
}

TEST_CASE("derive_nwk, known EUI64s: matches satellite.rs", "[zigbee]")
{
    const uint8_t zeros[8] = {0}, ones[8] = {255, 255, 255, 255, 255, 255, 255, 255}, seq[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    const uint8_t me[8] = {0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28};

    TEST_ASSERT_EQUAL_HEX16(0xea85, zigbee_satellite_derive_nwk(zeros));
    TEST_ASSERT_EQUAL_HEX16(0x66f4, zigbee_satellite_derive_nwk(ones));
    TEST_ASSERT_EQUAL_HEX16(0x4f8a, zigbee_satellite_derive_nwk(seq));
    TEST_ASSERT_EQUAL_HEX16(GOLD_NWK, zigbee_satellite_derive_nwk(me));
}

TEST_CASE("derive_nwk, any EUI64: never 0x0000 or 0xFFF8 and above", "[zigbee]")
{
    uint8_t eui[8] = {0};

    for (uint32_t i = 0; i < 200000; i++) {
        memcpy(eui, &i, sizeof(i));
        const uint16_t nwk = zigbee_satellite_derive_nwk(eui);

        TEST_ASSERT_TRUE(nwk != 0 && nwk < 0xFFF8);
    }
}

TEST_CASE("clock_counter, known time: seconds since 2026-01-01 times 10", "[zigbee]")
{
    TEST_ASSERT_EQUAL_UINT32(10, zigbee_satellite_clock_counter(1767225601));
    TEST_ASSERT_EQUAL_UINT32(0, zigbee_satellite_clock_counter(1000));
}

TEST_CASE("start: asks the stack for its EUI64", "[zigbee]")
{
    begin();
    TEST_ASSERT_EQUAL(1, t.count);
    TEST_ASSERT_EQUAL(sizeof(GOLD_GET_HW), t.lens[0]);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(GOLD_GET_HW, t.frames[0], sizeof(GOLD_GET_HW));
}

TEST_CASE("start, no answer within 10 s: fails naming the request", "[zigbee]")
{
    begin();
    t.now = ZIGBEE_SATELLITE_REQUEST_TIMEOUT_MS;
    zigbee_satellite_on_tick(&s);
    TEST_ASSERT_EQUAL(ZIGBEE_SATELLITE_FAILED, s.state);
    TEST_ASSERT_EQUAL_STRING("no answer to GetHwAddress", s.reason);
}

TEST_CASE("run, clock not valid: waits and requests nothing", "[zigbee]")
{
    begin();
    zigbee_satellite_on_frame(&s, GOLD_RSP_HW, sizeof(GOLD_RSP_HW));
    t.now = 1000;
    zigbee_satellite_on_tick(&s);
    TEST_ASSERT_EQUAL(ZIGBEE_SATELLITE_WAITING_CLOCK, s.state);
    TEST_ASSERT_EQUAL(0, t.netinfo_requests);
    TEST_ASSERT_EQUAL(1, t.count);
}

TEST_CASE("run, clock not valid after 60 s: fails with no clock", "[zigbee]")
{
    begin();
    zigbee_satellite_on_frame(&s, GOLD_RSP_HW, sizeof(GOLD_RSP_HW));
    t.now = ZIGBEE_SATELLITE_CLOCK_TIMEOUT_MS;
    zigbee_satellite_on_tick(&s);
    TEST_ASSERT_EQUAL(ZIGBEE_SATELLITE_FAILED, s.state);
    TEST_ASSERT_EQUAL_STRING("no clock", s.reason);
}

TEST_CASE("run, no coordinator: asks each second", "[zigbee]")
{
    to_waiting_coordinator();
    TEST_ASSERT_EQUAL(1, t.netinfo_requests);
    t.now = 500;
    zigbee_satellite_on_tick(&s);
    TEST_ASSERT_EQUAL(1, t.netinfo_requests);
    t.now = 1000;
    zigbee_satellite_on_tick(&s);
    TEST_ASSERT_EQUAL(2, t.netinfo_requests);
    TEST_ASSERT_EQUAL(ZIGBEE_SATELLITE_WAITING_COORDINATOR, s.state);
}

TEST_CASE("run, no settings after 60 s: fails with no coordinator", "[zigbee]")
{
    to_waiting_coordinator();
    t.now = ZIGBEE_SATELLITE_COORDINATOR_TIMEOUT_MS;
    zigbee_satellite_on_tick(&s);
    TEST_ASSERT_EQUAL(ZIGBEE_SATELLITE_FAILED, s.state);
    TEST_ASSERT_EQUAL_STRING("no coordinator", s.reason);
}

TEST_CASE("run, settings received: sends Configure as the Rust encoder writes it", "[zigbee]")
{
    uint8_t expected[sizeof(GOLD_CONFIGURE)];
    const uint32_t counter = zigbee_satellite_clock_counter(NOW_UNIX);

    to_configuring();
    memcpy(expected, GOLD_CONFIGURE, sizeof(expected));
    memcpy(&expected[3 + 2 + 39], &counter, 4);
    memcpy(&expected[3 + 2 + 78], &counter, 4);
    TEST_ASSERT_EQUAL(ZIGBEE_SATELLITE_CONFIGURING, s.state);
    TEST_ASSERT_EQUAL(2, t.count);
    TEST_ASSERT_EQUAL(sizeof(expected), t.lens[1]);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, t.frames[1], sizeof(expected));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(&GOLD_NETINFO[4], t.epan, 8);
}

TEST_CASE("run, Configure answered: sends StartNetwork", "[zigbee]")
{
    to_configuring();
    zigbee_satellite_on_frame(&s, GOLD_RSP_EMPTY_CONFIGURE, sizeof(GOLD_RSP_EMPTY_CONFIGURE));
    TEST_ASSERT_EQUAL(3, t.count);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(GOLD_START, t.frames[2], sizeof(GOLD_START));
}

TEST_CASE("run, StartNetwork answered: announces and runs", "[zigbee]")
{
    to_running();
    TEST_ASSERT_EQUAL(ZIGBEE_SATELLITE_RUNNING, s.state);
    TEST_ASSERT_EQUAL(1, t.announces);
    TEST_ASSERT_EQUAL_HEX16(GOLD_NWK, t.announced_nwk);
    TEST_ASSERT_EQUAL(4, t.count);
    TEST_ASSERT_EQUAL(sizeof(GOLD_ANNCE), t.lens[3]);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(GOLD_ANNCE, t.frames[3], sizeof(GOLD_ANNCE));
}

TEST_CASE("run, malformed settings or coordinator not started: keeps waiting", "[zigbee]")
{
    to_waiting_coordinator();
    zigbee_satellite_on_netinfo(&s, GOLD_NETINFO, 10);
    zigbee_satellite_on_netinfo(&s, GOLD_NETINFO_NOT_STARTED, sizeof(GOLD_NETINFO_NOT_STARTED));
    TEST_ASSERT_EQUAL(ZIGBEE_SATELLITE_WAITING_COORDINATOR, s.state);
    TEST_ASSERT_EQUAL(1, t.count);
}

TEST_CASE("run, StartNetwork refused: fails with the status", "[zigbee]")
{
    to_configuring();
    zigbee_satellite_on_frame(&s, GOLD_RSP_EMPTY_CONFIGURE, sizeof(GOLD_RSP_EMPTY_CONFIGURE));
    zigbee_satellite_on_frame(&s, GOLD_RSP_REFUSED_START, sizeof(GOLD_RSP_REFUSED_START));
    TEST_ASSERT_EQUAL(ZIGBEE_SATELLITE_FAILED, s.state);
    TEST_ASSERT_EQUAL_STRING("command 0x16 refused: status 0x31", s.reason);
}

TEST_CASE("run, no response to a request within 10 s: fails naming it", "[zigbee]")
{
    to_configuring();
    t.now += ZIGBEE_SATELLITE_REQUEST_TIMEOUT_MS;
    zigbee_satellite_on_tick(&s);
    TEST_ASSERT_EQUAL(ZIGBEE_SATELLITE_FAILED, s.state);
    TEST_ASSERT_EQUAL_STRING("no answer to Configure", s.reason);
}

TEST_CASE("on_received_aps, Active_EP_req for our NWK: answers with no endpoints", "[zigbee]")
{
    uint8_t expected[sizeof(GOLD_ACTIVE_EP_RSP)];

    to_running();
    zigbee_satellite_on_frame(&s, GOLD_RX_ACTIVE_EP_REQ, sizeof(GOLD_RX_ACTIVE_EP_REQ));
    TEST_ASSERT_EQUAL(5, t.count);
    memcpy(expected, GOLD_ACTIVE_EP_RSP, sizeof(expected));
    TEST_ASSERT_EQUAL(sizeof(expected), t.lens[4]);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, t.frames[4], sizeof(expected));
}

TEST_CASE("on_received_aps, Node_Desc_req for our NWK: answers as a router", "[zigbee]")
{
    uint8_t req[sizeof(GOLD_RX_ACTIVE_EP_REQ)];

    to_running();
    memcpy(req, GOLD_RX_ACTIVE_EP_REQ, sizeof(req));
    req[3 + 9] = 0x02; // Node_Desc_req
    zigbee_satellite_on_frame(&s, req, sizeof(req));
    TEST_ASSERT_EQUAL(5, t.count);
    const uint8_t *asdu = &t.frames[4][t.lens[4] - 17];

    TEST_ASSERT_EQUAL_HEX8(0x02, t.frames[4][3 + 13]); // response cluster, low byte
    TEST_ASSERT_EQUAL_HEX8(0x80, t.frames[4][3 + 14]);
    TEST_ASSERT_EQUAL_HEX8(0x01, asdu[4]);             // logical type: router
    TEST_ASSERT_EQUAL_HEX8(0x8E, asdu[6]);
}

TEST_CASE("on_received_aps, another NWK or cluster: sends nothing", "[zigbee]")
{
    uint8_t req[sizeof(GOLD_RX_ACTIVE_EP_REQ)];

    to_running();
    memcpy(req, GOLD_RX_ACTIVE_EP_REQ, sizeof(req));
    req[sizeof(req) - 1] ^= 0xFF; // NWK of interest
    zigbee_satellite_on_frame(&s, req, sizeof(req));
    memcpy(req, GOLD_RX_ACTIVE_EP_REQ, sizeof(req));
    req[3 + 9] = 0x31; // Mgmt_Lqi_req: the stack's
    zigbee_satellite_on_frame(&s, req, sizeof(req));
    TEST_ASSERT_EQUAL(4, t.count);
}
