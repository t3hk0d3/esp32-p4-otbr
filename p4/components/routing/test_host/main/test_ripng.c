#include <string.h>
#include "unity.h"
#include "routing.h"
#include "vectors.h"

/* The prefixes the scapy vectors were built with (tools/routing_vectors.py). */
#define P0 {{0x20, 0x01, 0x0d, 0xb8, 0x00, 0x42, 0x01, 0x00}} /* 2001:db8:42:100::/64 */
#define P1 {{0xfd, 0x98, 0x26, 0x56, 0x47, 0x40, 0x00, 0x01}} /* fd98:2656:4740:1::/64 */

static const routing_prefix64_t PFX0 = P0;
static const routing_prefix64_t PFX1 = P1;

static const routing_prefix_set_t SET_NONE = {.count = 0};
static const routing_prefix_set_t SET_P0 = {.p = {P0}, .count = 1};
static const routing_prefix_set_t SET_P1 = {.p = {P1}, .count = 1};
static const routing_prefix_set_t SET_BOTH = {.p = {P0, P1}, .count = 2};

#define FILL 0xa5
#define NONE UINT32_MAX
#define T0 1000u

static uint8_t s_buf[ROUTING_RIPNG_MAX_PAYLOAD + 64];

static void fill_buf(void)
{
    memset(s_buf, FILL, sizeof(s_buf));
}

/* --- injected random source: returns the queued values in order, then 0 --- */

static uint32_t s_rand[8];
static size_t s_rand_len;
static size_t s_rand_pos;

static void rand_queue(size_t n, const uint32_t *values)
{
    memcpy(s_rand, values, n * sizeof(values[0]));
    s_rand_len = n;
    s_rand_pos = 0;
}

static uint32_t fake_rand32(void)
{
    return s_rand_pos < s_rand_len ? s_rand[s_rand_pos++] : 0;
}

/* --- scheduler helpers --- */

static routing_ripng_sched_t s;
static routing_ripng_rte_t s_out[ROUTING_RIPNG_MAX_RTES];

static void start(const routing_prefix_set_t *set, uint32_t now)
{
    memset(&s, 0xaa, sizeof(s));
    rand_queue(0, NULL);
    routing_ripng_sched_start(&s, set, now, fake_rand32);
}

static size_t poll_at(uint32_t now)
{
    memset(s_out, 0xaa, sizeof(s_out));
    return routing_ripng_sched_poll(&s, now, s_out, ROUTING_RIPNG_MAX_RTES);
}

/* The metric `prefix` has in the first `n` entries of s_out, or 0 when it is absent. */
static uint8_t metric_of(size_t n, const routing_prefix64_t *prefix)
{
    for (size_t i = 0; i < n; i++) {
        if (memcmp(s_out[i].prefix.b, prefix->b, sizeof(prefix->b)) == 0) {
            return s_out[i].metric;
        }
    }
    return 0;
}

/* Starts with SET_BOTH at T0 and sends the initial full update; the next regular one is due at T0 + 30000. */
static void started_both(void)
{
    start(&SET_BOTH, T0);
    TEST_ASSERT_EQUAL_size_t(2, poll_at(T0));
}

/* --- encode_response --- */

TEST_CASE("Announcement set of two prefixes: writes command 2, version 1, and one RTE per prefix with length 64, tag "
          "0, metric 1", "[routing]")
{
    const routing_ripng_rte_t rtes[] = {{P0, 1}, {P1, 1}};
    size_t consumed = 99;

    fill_buf();
    size_t n = routing_ripng_encode_response(s_buf, sizeof(s_buf), rtes, 2, &consumed);

    TEST_ASSERT_EQUAL_size_t(sizeof(RIPNG_RESPONSE_2), n);
    TEST_ASSERT_EQUAL_size_t(2, consumed);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(RIPNG_RESPONSE_2, s_buf, sizeof(RIPNG_RESPONSE_2));
    TEST_ASSERT_EQUAL_HEX8(FILL, s_buf[n]);
}

TEST_CASE("Withdrawn prefix: writes its RTE with metric 16", "[routing]")
{
    const routing_ripng_rte_t rtes[] = {{P0, 1}, {P1, ROUTING_RIPNG_INFINITY}};
    size_t consumed = 99;

    fill_buf();
    size_t n = routing_ripng_encode_response(s_buf, sizeof(s_buf), rtes, 2, &consumed);

    TEST_ASSERT_EQUAL_size_t(sizeof(RIPNG_RESPONSE_WITHDRAWN), n);
    TEST_ASSERT_EQUAL_size_t(2, consumed);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(RIPNG_RESPONSE_WITHDRAWN, s_buf, sizeof(RIPNG_RESPONSE_WITHDRAWN));
    TEST_ASSERT_EQUAL_HEX8(FILL, s_buf[n]);
}

TEST_CASE("More entries than fit one message: splits them across messages that each fit the backbone MTU",
          "[routing]")
{
    routing_ripng_rte_t rtes[62];
    size_t consumed = 99;

    for (size_t i = 0; i < 62; i++) {
        rtes[i] = (routing_ripng_rte_t) {
            .prefix = {{0x20, 0x01, 0x0d, 0xb8, 0, (uint8_t)i, 0, 0}}, .metric = 1
        };
    }

    fill_buf();
    size_t n = routing_ripng_encode_response(s_buf, ROUTING_RIPNG_MAX_PAYLOAD, rtes, 62, &consumed);
    TEST_ASSERT_EQUAL_size_t(61, consumed);
    TEST_ASSERT_EQUAL_size_t(4 + 61 * 20, n);
    TEST_ASSERT_LESS_OR_EQUAL_size_t(ROUTING_RIPNG_MAX_PAYLOAD, n);
    TEST_ASSERT_EQUAL_HEX8(FILL, s_buf[n]);
    TEST_ASSERT_EQUAL_HEX8(60, s_buf[4 + 60 * 20 + 5]); /* the last RTE is entry 60 */

    fill_buf();
    n = routing_ripng_encode_response(s_buf, ROUTING_RIPNG_MAX_PAYLOAD, rtes + 61, 1, &consumed);
    TEST_ASSERT_EQUAL_size_t(1, consumed);
    TEST_ASSERT_EQUAL_size_t(4 + 20, n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(RIPNG_RESPONSE_2, s_buf, 4); /* a header of its own */
    TEST_ASSERT_EQUAL_HEX8(61, s_buf[4 + 5]);
}

TEST_CASE("Buffer too small for one RTE, or no entries: returns 0 and writes nothing", "[routing]")
{
    const routing_ripng_rte_t rtes[] = {{P0, 1}};
    size_t consumed = 99;

    fill_buf();
    TEST_ASSERT_EQUAL_size_t(0, routing_ripng_encode_response(s_buf, 23, rtes, 1, &consumed));
    TEST_ASSERT_EQUAL_size_t(0, consumed);
    consumed = 99;
    TEST_ASSERT_EQUAL_size_t(0, routing_ripng_encode_response(s_buf, sizeof(s_buf), rtes, 0, &consumed));
    TEST_ASSERT_EQUAL_size_t(0, consumed);
    for (size_t i = 0; i < sizeof(s_buf); i++) {
        TEST_ASSERT_EQUAL_HEX8(FILL, s_buf[i]);
    }
}

/* --- decode --- */

TEST_CASE("Datagram shorter than the 4-byte header, or version not 1: is dropped and counted", "[routing]")
{
    TEST_ASSERT_EQUAL(ROUTING_RIPNG_INVALID, routing_ripng_classify(RIPNG_RX_SHORT, sizeof(RIPNG_RX_SHORT)));
    TEST_ASSERT_EQUAL(ROUTING_RIPNG_INVALID, routing_ripng_classify(RIPNG_RX_SHORT, 0));
    TEST_ASSERT_EQUAL(ROUTING_RIPNG_INVALID,
                      routing_ripng_classify(RIPNG_RX_VERSION_2, sizeof(RIPNG_RX_VERSION_2)));
}

TEST_CASE("Command other than request or response: is dropped and counted", "[routing]")
{
    uint8_t msg[sizeof(RIPNG_RESPONSE_2)];

    memcpy(msg, RIPNG_RESPONSE_2, sizeof(msg));
    msg[0] = 3;
    TEST_ASSERT_EQUAL(ROUTING_RIPNG_INVALID, routing_ripng_classify(msg, sizeof(msg)));
    msg[0] = 0;
    TEST_ASSERT_EQUAL(ROUTING_RIPNG_INVALID, routing_ripng_classify(msg, sizeof(msg)));
}

TEST_CASE("Datagram truncated mid-RTE (length not 4 + 20 n): is dropped and counted", "[routing]")
{
    TEST_ASSERT_EQUAL(ROUTING_RIPNG_INVALID,
                      routing_ripng_classify(RIPNG_RX_TRUNCATED, sizeof(RIPNG_RX_TRUNCATED)));
    TEST_ASSERT_EQUAL(ROUTING_RIPNG_INVALID, routing_ripng_classify(RIPNG_REQUEST_ALL, sizeof(RIPNG_REQUEST_ALL) - 1));
}

TEST_CASE("Response from a neighbour: counts it in responses_dropped and changes nothing", "[routing]")
{
    TEST_ASSERT_EQUAL(ROUTING_RIPNG_RESPONSE, routing_ripng_classify(RIPNG_RESPONSE_2, sizeof(RIPNG_RESPONSE_2)));
    fill_buf();
    TEST_ASSERT_EQUAL_size_t(0, routing_ripng_encode_answer(RIPNG_RESPONSE_2, sizeof(RIPNG_RESPONSE_2), &SET_BOTH,
                                                            s_buf, sizeof(s_buf)));
    TEST_ASSERT_EQUAL_HEX8(FILL, s_buf[0]);
}

TEST_CASE("Whole-table request (one RTE, prefix ::/0, length 0, metric 16): returns a full-table response addressed "
          "to the requester's address and port", "[routing]")
{
    routing_ripng_rte_t out[4];
    size_t consumed;

    TEST_ASSERT_EQUAL(ROUTING_RIPNG_REQUEST_ALL, routing_ripng_classify(RIPNG_REQUEST_ALL, sizeof(RIPNG_REQUEST_ALL)));
    size_t n = routing_ripng_table(&SET_BOTH, out, 4);

    TEST_ASSERT_EQUAL_size_t(2, n);
    fill_buf();
    size_t len = routing_ripng_encode_response(s_buf, sizeof(s_buf), out, n, &consumed);
    TEST_ASSERT_EQUAL_size_t(sizeof(RIPNG_RESPONSE_2), len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(RIPNG_RESPONSE_2, s_buf, sizeof(RIPNG_RESPONSE_2));
}

TEST_CASE("Whole-table request: is not answered as a request for specific prefixes", "[routing]")
{
    fill_buf();
    TEST_ASSERT_EQUAL_size_t(0, routing_ripng_encode_answer(RIPNG_REQUEST_ALL, sizeof(RIPNG_REQUEST_ALL), &SET_BOTH,
                                                            s_buf, sizeof(s_buf)));
    TEST_ASSERT_EQUAL_HEX8(FILL, s_buf[0]);
}

/* RTE `i` of a message: its byte at `off` within the 20-byte entry. */
#define RTE_BYTE(msg, i, off) ((msg)[ROUTING_RIPNG_HDR_LEN + (i) * ROUTING_RIPNG_RTE_LEN + (off)])

TEST_CASE("Request for specific prefixes: returns those entries, metric 1 for announced ones and 16 for the others",
          "[routing]")
{
    TEST_ASSERT_EQUAL(ROUTING_RIPNG_REQUEST_SOME,
                      routing_ripng_classify(RIPNG_REQUEST_SOME, sizeof(RIPNG_REQUEST_SOME)));
    fill_buf();
    size_t n = routing_ripng_encode_answer(RIPNG_REQUEST_SOME, sizeof(RIPNG_REQUEST_SOME), &SET_BOTH, s_buf,
                                           sizeof(s_buf));

    TEST_ASSERT_EQUAL_size_t(sizeof(RIPNG_ANSWER_SOME), n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(RIPNG_ANSWER_SOME, s_buf, sizeof(RIPNG_ANSWER_SOME));
    TEST_ASSERT_EQUAL_HEX8(FILL, s_buf[n]);
    TEST_ASSERT_EQUAL_UINT8(2, s_buf[0]);
    /* every RTE echoed (prefix, tag, length) with its metric filled in */
    for (size_t i = 0; i < 5; i++) {
        TEST_ASSERT_EQUAL_HEX8_ARRAY(&RTE_BYTE(RIPNG_REQUEST_SOME, i, 0), &RTE_BYTE(s_buf, i, 0), 19);
    }
    TEST_ASSERT_EQUAL_UINT8(1, RTE_BYTE(s_buf, 0, 19));   /* P0 /64: announced */
    TEST_ASSERT_EQUAL_UINT8(16, RTE_BYTE(s_buf, 1, 19));  /* unknown /64, tag 7 kept */
    TEST_ASSERT_EQUAL_UINT8(7, RTE_BYTE(s_buf, 1, 17));
    TEST_ASSERT_EQUAL_UINT8(56, RTE_BYTE(s_buf, 2, 18));  /* P0's first 8 bytes as a /56 */
    TEST_ASSERT_EQUAL_UINT8(16, RTE_BYTE(s_buf, 2, 19));
    TEST_ASSERT_EQUAL_UINT8(128, RTE_BYTE(s_buf, 3, 18)); /* a /128 inside P0 */
    TEST_ASSERT_EQUAL_UINT8(16, RTE_BYTE(s_buf, 3, 19));
    TEST_ASSERT_EQUAL_UINT8(64, RTE_BYTE(s_buf, 4, 18));  /* P0 with bytes 8-15 not zero */
    TEST_ASSERT_EQUAL_UINT8(16, RTE_BYTE(s_buf, 4, 19));
}

TEST_CASE("Request with the ::/0 metric-16 RTE followed by another RTE: is a request for specific prefixes",
          "[routing]")
{
    uint8_t msg[ROUTING_RIPNG_HDR_LEN + 2 * ROUTING_RIPNG_RTE_LEN];

    memcpy(msg, RIPNG_REQUEST_ALL, sizeof(RIPNG_REQUEST_ALL));
    memcpy(&msg[sizeof(RIPNG_REQUEST_ALL)], &RIPNG_REQUEST_SOME[ROUTING_RIPNG_HDR_LEN], ROUTING_RIPNG_RTE_LEN);
    TEST_ASSERT_EQUAL(ROUTING_RIPNG_REQUEST_SOME, routing_ripng_classify(msg, sizeof(msg)));
}

TEST_CASE("Request for specific prefixes larger than cap, or with no RTEs: returns 0 and writes nothing", "[routing]")
{
    fill_buf();
    TEST_ASSERT_EQUAL_size_t(0, routing_ripng_encode_answer(RIPNG_REQUEST_SOME, sizeof(RIPNG_REQUEST_SOME), &SET_BOTH,
                                                            s_buf, sizeof(RIPNG_REQUEST_SOME) - 1));
    TEST_ASSERT_EQUAL_size_t(0, routing_ripng_encode_answer(RIPNG_REQUEST_SOME, ROUTING_RIPNG_HDR_LEN, &SET_BOTH,
                                                            s_buf, sizeof(s_buf)));
    for (size_t i = 0; i < sizeof(s_buf); i++) {
        TEST_ASSERT_EQUAL_HEX8(FILL, s_buf[i]);
    }
}

/* --- schedule --- */

TEST_CASE("Started: sends a full response at once", "[routing]")
{
    start(&SET_BOTH, T0);

    TEST_ASSERT_EQUAL_UINT32(T0, routing_ripng_sched_next_deadline_ms(&s));
    size_t n = poll_at(T0);
    TEST_ASSERT_EQUAL_size_t(2, n);
    TEST_ASSERT_EQUAL_UINT8(1, metric_of(n, &PFX0));
    TEST_ASSERT_EQUAL_UINT8(1, metric_of(n, &PFX1));
    TEST_ASSERT_EQUAL_size_t(0, poll_at(T0));
}

TEST_CASE("Started: then every 30 s plus a random 0..5 s", "[routing]")
{
    const uint32_t r[] = {0, 5000, 5001 + 4999};

    start(&SET_BOTH, T0);
    rand_queue(3, r);

    TEST_ASSERT_EQUAL_size_t(2, poll_at(T0)); /* offset 0 */
    TEST_ASSERT_EQUAL_UINT32(T0 + 30000, routing_ripng_sched_next_deadline_ms(&s));
    TEST_ASSERT_EQUAL_size_t(0, poll_at(T0 + 29999));
    TEST_ASSERT_EQUAL_size_t(2, poll_at(T0 + 30000)); /* offset 5000 */
    TEST_ASSERT_EQUAL_UINT32(T0 + 65000, routing_ripng_sched_next_deadline_ms(&s));
    TEST_ASSERT_EQUAL_size_t(0, poll_at(T0 + 64999));
    TEST_ASSERT_EQUAL_size_t(2, poll_at(T0 + 65000)); /* offset 10000 % 5001 = 4999 */
    TEST_ASSERT_EQUAL_UINT32(T0 + 99999, routing_ripng_sched_next_deadline_ms(&s));
}

TEST_CASE("Clock wraps past UINT32_MAX: the next regular update still falls due 30 s later", "[routing]")
{
    const uint32_t t = UINT32_MAX - 10000;

    start(&SET_BOTH, t);
    TEST_ASSERT_EQUAL_size_t(2, poll_at(t));
    TEST_ASSERT_EQUAL_UINT32(t + 30000, routing_ripng_sched_next_deadline_ms(&s));
    TEST_ASSERT_EQUAL_size_t(0, poll_at(UINT32_MAX));
    TEST_ASSERT_EQUAL_size_t(0, poll_at(t + 29999));
    TEST_ASSERT_EQUAL_size_t(2, poll_at(t + 30000));
}

TEST_CASE("Announcement set changed: sends a triggered response with only the changed entries", "[routing]")
{
    start(&SET_P0, T0);
    TEST_ASSERT_EQUAL_size_t(1, poll_at(T0));

    routing_ripng_sched_set_changed(&s, &SET_P1, T0 + 1000);
    TEST_ASSERT_EQUAL_UINT32(T0 + 1000, routing_ripng_sched_next_deadline_ms(&s));
    size_t n = poll_at(T0 + 1000);
    TEST_ASSERT_EQUAL_size_t(2, n);
    TEST_ASSERT_EQUAL_UINT8(1, metric_of(n, &PFX1));  /* added */
    TEST_ASSERT_EQUAL_UINT8(16, metric_of(n, &PFX0)); /* removed */

    routing_ripng_sched_set_changed(&s, &SET_BOTH, T0 + 10000);
    n = poll_at(T0 + 10000);
    TEST_ASSERT_EQUAL_size_t(1, n); /* P1 unchanged, so left out */
    TEST_ASSERT_EQUAL_UINT8(1, metric_of(n, &PFX0));
}

TEST_CASE("Announcement set changed more than 2^31 ms after the previous triggered update: sends it at once",
          "[routing]")
{
    started_both();
    routing_ripng_sched_set_changed(&s, &SET_P0, T0 + 1000);
    TEST_ASSERT_EQUAL_size_t(1, poll_at(T0 + 1000));

    /* regular updates only, until the clock is more than 2^31 ms past that triggered update */
    uint32_t now = T0;
    for (uint32_t i = 0; i < 0x80000000u / ROUTING_RIPNG_UPDATE_MS + 100; i++) {
        now += ROUTING_RIPNG_UPDATE_MS;
        TEST_ASSERT_NOT_EQUAL(0, poll_at(now));
    }

    routing_ripng_sched_set_changed(&s, &SET_BOTH, now + 1000);
    TEST_ASSERT_EQUAL_UINT32(now + 1000, routing_ripng_sched_next_deadline_ms(&s));
    size_t n = poll_at(now + 1000);
    TEST_ASSERT_EQUAL_size_t(1, n);
    TEST_ASSERT_EQUAL_UINT8(1, metric_of(n, &PFX1));
}

TEST_CASE("Announcement set unchanged: sends no triggered update", "[routing]")
{
    started_both();
    routing_ripng_sched_set_changed(&s, &SET_BOTH, T0 + 1000);

    TEST_ASSERT_EQUAL_UINT32(T0 + 30000, routing_ripng_sched_next_deadline_ms(&s));
    TEST_ASSERT_EQUAL_size_t(0, poll_at(T0 + 1000));
}

TEST_CASE("Announcement set changed: no sooner than 5 s after the previous triggered one", "[routing]")
{
    start(&SET_NONE, T0);
    TEST_ASSERT_EQUAL_size_t(0, poll_at(T0)); /* nothing to announce */

    routing_ripng_sched_set_changed(&s, &SET_P0, T0 + 1000);
    TEST_ASSERT_EQUAL_size_t(1, poll_at(T0 + 1000));

    routing_ripng_sched_set_changed(&s, &SET_BOTH, T0 + 2000);
    TEST_ASSERT_EQUAL_UINT32(T0 + 6000, routing_ripng_sched_next_deadline_ms(&s));
    TEST_ASSERT_EQUAL_size_t(0, poll_at(T0 + 2000));
    routing_ripng_sched_set_changed(&s, &SET_P1, T0 + 4000); /* merges: P0 removed, P1 still added */
    TEST_ASSERT_EQUAL_UINT32(T0 + 6000, routing_ripng_sched_next_deadline_ms(&s));
    TEST_ASSERT_EQUAL_size_t(0, poll_at(T0 + 5999));

    size_t n = poll_at(T0 + 6000);
    TEST_ASSERT_EQUAL_size_t(2, n);
    TEST_ASSERT_EQUAL_UINT8(1, metric_of(n, &PFX1));
    TEST_ASSERT_EQUAL_UINT8(16, metric_of(n, &PFX0));
    TEST_ASSERT_EQUAL_size_t(0, poll_at(T0 + 6000));
}

TEST_CASE("Regular update due while a triggered one is deferred: carries everything and the trigger is dropped",
          "[routing]")
{
    started_both();
    routing_ripng_sched_set_changed(&s, &SET_P0, T0 + 26000);
    TEST_ASSERT_EQUAL_size_t(1, poll_at(T0 + 26000)); /* P1 at 16 */

    routing_ripng_sched_set_changed(&s, &SET_BOTH, T0 + 29000); /* deferred to T0 + 31000 */
    TEST_ASSERT_EQUAL_UINT32(T0 + 30000, routing_ripng_sched_next_deadline_ms(&s));
    size_t n = poll_at(T0 + 30000);
    TEST_ASSERT_EQUAL_size_t(2, n);
    TEST_ASSERT_EQUAL_UINT8(1, metric_of(n, &PFX0));
    TEST_ASSERT_EQUAL_UINT8(1, metric_of(n, &PFX1));

    TEST_ASSERT_EQUAL_UINT32(T0 + 60000, routing_ripng_sched_next_deadline_ms(&s));
    TEST_ASSERT_EQUAL_size_t(0, poll_at(T0 + 31000));
}

TEST_CASE("Prefix removed: sends it with metric 16 in the triggered update and the next 4 regular updates",
          "[routing]")
{
    started_both();
    routing_ripng_sched_set_changed(&s, &SET_P0, T0 + 1000);
    size_t n = poll_at(T0 + 1000);
    TEST_ASSERT_EQUAL_size_t(1, n);
    TEST_ASSERT_EQUAL_UINT8(16, metric_of(n, &PFX1));

    for (uint32_t i = 1; i <= 4; i++) {
        n = poll_at(T0 + i * 30000);
        TEST_ASSERT_EQUAL_size_t(2, n);
        TEST_ASSERT_EQUAL_UINT8(1, metric_of(n, &PFX0));
        TEST_ASSERT_EQUAL_UINT8(16, metric_of(n, &PFX1));
    }
}

TEST_CASE("Prefix removed: then leaves it out", "[routing]")
{
    started_both();
    routing_ripng_sched_set_changed(&s, &SET_P0, T0 + 1000);
    TEST_ASSERT_EQUAL_size_t(1, poll_at(T0 + 1000));
    for (uint32_t i = 1; i <= 4; i++) {
        TEST_ASSERT_EQUAL_size_t(2, poll_at(T0 + i * 30000));
    }

    size_t n = poll_at(T0 + 5 * 30000);
    TEST_ASSERT_EQUAL_size_t(1, n);
    TEST_ASSERT_EQUAL_UINT8(1, metric_of(n, &PFX0));
    TEST_ASSERT_EQUAL_UINT8(0, metric_of(n, &PFX1));
}

TEST_CASE("Prefix removed and the update goes out as a regular one: it counts as the triggered one, 4 more follow",
          "[routing]")
{
    started_both();
    routing_ripng_sched_set_changed(&s, &SET_P0, T0 + 26000);
    TEST_ASSERT_EQUAL_size_t(1, poll_at(T0 + 26000));
    routing_ripng_sched_set_changed(&s, &SET_NONE, T0 + 29000); /* P0's trigger deferred, then replaced */

    for (uint32_t i = 1; i <= 5; i++) {
        size_t n = poll_at(T0 + i * 30000);
        TEST_ASSERT_EQUAL_UINT8(16, metric_of(n, &PFX0));
    }
    TEST_ASSERT_EQUAL_UINT8(0, metric_of(poll_at(T0 + 6 * 30000), &PFX0));
}

TEST_CASE("Prefix re-announced while still sent at metric 16: goes back to metric 1 at once", "[routing]")
{
    started_both();
    routing_ripng_sched_set_changed(&s, &SET_P0, T0 + 1000);
    TEST_ASSERT_EQUAL_size_t(1, poll_at(T0 + 1000));
    TEST_ASSERT_EQUAL_UINT8(16, metric_of(poll_at(T0 + 30000), &PFX1));

    routing_ripng_sched_set_changed(&s, &SET_BOTH, T0 + 31000);
    size_t n = poll_at(T0 + 31000);
    TEST_ASSERT_EQUAL_size_t(1, n);
    TEST_ASSERT_EQUAL_UINT8(1, metric_of(n, &PFX1));

    n = poll_at(T0 + 60000);
    TEST_ASSERT_EQUAL_size_t(2, n); /* no metric-16 copy left behind */
    TEST_ASSERT_EQUAL_UINT8(1, metric_of(n, &PFX1));
}

TEST_CASE("Withdrawn list full: the entry closest to expiry gives way", "[routing]")
{
    routing_prefix_set_t set = {.count = 1};
    routing_prefix64_t first = {{0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0}};

    set.p[0] = first;
    start(&set, T0);
    TEST_ASSERT_EQUAL_size_t(1, poll_at(T0));
    /* Withdraw 1 prefix, let it age by one regular update, then withdraw MAX_WITHDRAWN more. */
    routing_ripng_sched_set_changed(&s, &SET_NONE, T0 + 1000);
    poll_at(T0 + 1000);
    poll_at(T0 + 30000);
    uint32_t t = T0 + 31000;
    for (uint8_t i = 1; i <= ROUTING_RIPNG_MAX_WITHDRAWN; i++) {
        set.p[0].b[5] = i;
        routing_ripng_sched_set_changed(&s, &set, t);
        routing_ripng_sched_set_changed(&s, &SET_NONE, t);
        poll_at(t);
        t += ROUTING_RIPNG_TRIGGER_GAP_MS;
    }

    size_t n = poll_at(T0 + 60000);
    TEST_ASSERT_EQUAL_size_t(ROUTING_RIPNG_MAX_WITHDRAWN, n);
    TEST_ASSERT_EQUAL_UINT8(0, metric_of(n, &first));
    for (uint8_t i = 1; i <= ROUTING_RIPNG_MAX_WITHDRAWN; i++) {
        set.p[0].b[5] = i;
        TEST_ASSERT_EQUAL_UINT8(16, metric_of(n, &set.p[0]));
    }
}

TEST_CASE("Stopped (method changed or border router stopped): sends every announced prefix with metric 16 once, at "
          "once", "[routing]")
{
    started_both();
    routing_ripng_sched_set_changed(&s, &SET_P0, T0 + 1000);
    TEST_ASSERT_EQUAL_size_t(1, poll_at(T0 + 1000)); /* P1 now withdrawing */

    memset(s_out, 0xaa, sizeof(s_out));
    size_t n = routing_ripng_sched_stop(&s, s_out, ROUTING_RIPNG_MAX_RTES);
    TEST_ASSERT_EQUAL_size_t(2, n);
    TEST_ASSERT_EQUAL_UINT8(16, metric_of(n, &PFX0));
    TEST_ASSERT_EQUAL_UINT8(16, metric_of(n, &PFX1));
    TEST_ASSERT_EQUAL_size_t(0, routing_ripng_sched_stop(&s, s_out, ROUTING_RIPNG_MAX_RTES));
}

TEST_CASE("Stopped (method changed or border router stopped): sends nothing after", "[routing]")
{
    started_both();
    routing_ripng_sched_stop(&s, s_out, ROUTING_RIPNG_MAX_RTES);

    TEST_ASSERT_EQUAL_UINT32(NONE, routing_ripng_sched_next_deadline_ms(&s));
    routing_ripng_sched_set_changed(&s, &SET_P1, T0 + 1000);
    TEST_ASSERT_EQUAL_UINT32(NONE, routing_ripng_sched_next_deadline_ms(&s));
    TEST_ASSERT_EQUAL_size_t(0, poll_at(T0 + 1000));
    TEST_ASSERT_EQUAL_size_t(0, poll_at(T0 + 30000));
    TEST_ASSERT_EQUAL_size_t(0, poll_at(T0 + 120000));

    start(&SET_P1, T0 + 200000); /* until started again */
    TEST_ASSERT_EQUAL_size_t(1, poll_at(T0 + 200000));
}
