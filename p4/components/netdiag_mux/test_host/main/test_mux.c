#include <stdlib.h>
#include <string.h>

#include "mux.h"
#include "unity.h"

#define MAX_CALLS 16

// Fake send: records its arguments, returns a scripted code.
static int s_send_calls;
static otIp6Address s_send_dest;
static const uint8_t *s_send_tlvs;
static uint8_t s_send_count;
static otError s_send_ret;

static otError fake_send(const otIp6Address *dest, const uint8_t *tlvs, uint8_t count)
{
    s_send_calls++;
    s_send_dest = *dest;
    s_send_tlvs = tlvs;
    s_send_count = count;
    return s_send_ret;
}

static const netdiag_mux_ops_t s_ops = { .send = fake_send };

// Listener callbacks: each records its calls in call order.
typedef struct {
    int calls;
    otError error[MAX_CALLS];
    otMessage *msg[MAX_CALLS];
    const otMessageInfo *info[MAX_CALLS];
    void *ctx[MAX_CALLS];
} record_t;

static record_t s_rec[9];

static void record(record_t *r, otError error, otMessage *msg, const otMessageInfo *info, void *ctx)
{
    if (r->calls < MAX_CALLS) {
        r->error[r->calls] = error;
        r->msg[r->calls] = msg;
        r->info[r->calls] = info;
        r->ctx[r->calls] = ctx;
    }
    r->calls++;
}

static void cb_a(otError e, otMessage *m, const otMessageInfo *i, void *c)
{
    record(&s_rec[0], e, m, i, c);
}

static void cb_b(otError e, otMessage *m, const otMessageInfo *i, void *c)
{
    record(&s_rec[1], e, m, i, c);
}

static void cb_c(otError e, otMessage *m, const otMessageInfo *i, void *c)
{
    record(&s_rec[2], e, m, i, c);
}

static void cb_d(otError e, otMessage *m, const otMessageInfo *i, void *c)
{
    record(&s_rec[3], e, m, i, c);
}

static void cb_e(otError e, otMessage *m, const otMessageInfo *i, void *c)
{
    record(&s_rec[4], e, m, i, c);
}

static void cb_f(otError e, otMessage *m, const otMessageInfo *i, void *c)
{
    record(&s_rec[5], e, m, i, c);
}

static void cb_g(otError e, otMessage *m, const otMessageInfo *i, void *c)
{
    record(&s_rec[6], e, m, i, c);
}

static void cb_h(otError e, otMessage *m, const otMessageInfo *i, void *c)
{
    record(&s_rec[7], e, m, i, c);
}

static void cb_i(otError e, otMessage *m, const otMessageInfo *i, void *c)
{
    record(&s_rec[8], e, m, i, c);
}

static const otIp6Address *rloc_0400(void);
static const otIp6Address *rloc_0401(void);
static otError send(const otIp6Address *dest, otReceiveDiagnosticGetCallback cb, void *ctx, bool filter_peer);

/** A listener that records and then sends again (as the scan's child step does) from inside dispatch. */
static void cb_resend(otError e, otMessage *m, const otMessageInfo *i, void *c)
{
    record(&s_rec[4], e, m, i, c);
    send(rloc_0401(), cb_i, NULL, false); // a new callback and a re-send of a known one
    send(rloc_0401(), cb_resend, c, false);
}

static const uint8_t TLVS[] = { 0, 1, 8 };
static uint8_t s_msg_storage;
#define MSG ((otMessage *)&s_msg_storage)

/** fdde:ad00:beef:0:0:ff:fe00:<locator> (a locator address). */
static otIp6Address locator(uint16_t locator)
{
    otIp6Address a = { .mFields.m8 = { 0xfd, 0xde, 0xad, 0x00, 0xbe, 0xef, 0, 0, 0, 0, 0, 0xff, 0xfe, 0x00 } };
    a.mFields.m8[14] = (uint8_t)(locator >> 8);
    a.mFields.m8[15] = (uint8_t)locator;
    return a;
}

static const otIp6Address *rloc_0400(void)
{
    static otIp6Address a;
    a = locator(0x0400);
    return &a;
}

static const otIp6Address *rloc_0401(void)
{
    static otIp6Address a;
    a = locator(0x0401);
    return &a;
}

/** The leader anycast locator fdde:ad00:beef:0:0:ff:fe00:fc00. */
static const otIp6Address *aloc_leader(void)
{
    static otIp6Address a;
    a = locator(0xfc00);
    return &a;
}

/** ff03::2 (realm-local all routers). */
static const otIp6Address *multicast(void)
{
    static otIp6Address a = { .mFields.m8 = { 0xff, 0x03, [15] = 0x02 } };
    return &a;
}

static otMessageInfo from(const otIp6Address *peer)
{
    otMessageInfo info;

    memset(&info, 0, sizeof(info));
    info.mPeerAddr = *peer;
    return info;
}

static otError send(const otIp6Address *dest, otReceiveDiagnosticGetCallback cb, void *ctx, bool filter_peer)
{
    return netdiag_mux_core_send(&s_ops, dest, TLVS, sizeof(TLVS), cb, ctx, filter_peer);
}

static void reset(void)
{
    netdiag_mux_core_reset();
    memset(s_rec, 0, sizeof(s_rec));
    s_send_calls = 0;
    s_send_ret = OT_ERROR_NONE;
    memset(&s_send_dest, 0, sizeof(s_send_dest));
    s_send_tlvs = NULL;
    s_send_count = 0;
}

// netdiag_mux_core_send

TEST_CASE("On a send with a new callback: calls ops.send once with dest, tlvs and count", "[netdiag_mux]")
{
    reset();
    send(rloc_0400(), cb_a, NULL, true);
    TEST_ASSERT_EQUAL_INT(1, s_send_calls);
    TEST_ASSERT_EQUAL_MEMORY(rloc_0400(), &s_send_dest, sizeof(otIp6Address));
    TEST_ASSERT_EQUAL_PTR(TLVS, s_send_tlvs);
    TEST_ASSERT_EQUAL_UINT8(sizeof(TLVS), s_send_count);
}

TEST_CASE("On a send with a new callback: returns the result of ops.send", "[netdiag_mux]")
{
    reset();
    TEST_ASSERT_EQUAL_INT(OT_ERROR_NONE, send(rloc_0400(), cb_a, NULL, true));
    s_send_ret = OT_ERROR_BUSY;
    TEST_ASSERT_EQUAL_INT(OT_ERROR_BUSY, send(rloc_0400(), cb_b, NULL, true));
}

TEST_CASE("On a send with a new callback: registers the callback as a listener with its context", "[netdiag_mux]")
{
    reset();
    send(rloc_0400(), cb_a, (void *)7, false);
    otMessageInfo info = from(rloc_0400());
    netdiag_mux_core_dispatch(OT_ERROR_NONE, MSG, &info);
    TEST_ASSERT_EQUAL_INT(1, s_rec[0].calls);
    TEST_ASSERT_EQUAL_PTR((void *)7, s_rec[0].ctx[0]);
}

TEST_CASE("On a send with a known callback: keeps one listener with the latest context", "[netdiag_mux]")
{
    reset();
    send(rloc_0400(), cb_a, (void *)1, false);
    send(rloc_0401(), cb_a, (void *)2, false);
    otMessageInfo info = from(rloc_0400());
    netdiag_mux_core_dispatch(OT_ERROR_NONE, MSG, &info);
    TEST_ASSERT_EQUAL_INT(1, s_rec[0].calls);
    TEST_ASSERT_EQUAL_PTR((void *)2, s_rec[0].ctx[0]);
}

TEST_CASE("On a send with a known callback: does not use a listener slot", "[netdiag_mux]")
{
    reset();
    send(rloc_0400(), cb_a, NULL, false);
    send(rloc_0400(), cb_a, NULL, false);
    send(rloc_0400(), cb_b, NULL, false);
    send(rloc_0400(), cb_c, NULL, false);
    TEST_ASSERT_EQUAL_INT(OT_ERROR_NONE, send(rloc_0400(), cb_d, NULL, false));
}

TEST_CASE("On a filtered send to a unicast address: answers from that address reach the listener", "[netdiag_mux]")
{
    reset();
    send(rloc_0400(), cb_a, NULL, true);
    otMessageInfo info = from(rloc_0400());
    netdiag_mux_core_dispatch(OT_ERROR_NONE, MSG, &info);
    TEST_ASSERT_EQUAL_INT(1, s_rec[0].calls);
}

TEST_CASE("On a filtered send to a unicast address: answers from another address are not delivered", "[netdiag_mux]")
{
    reset();
    send(rloc_0400(), cb_a, NULL, true);
    otMessageInfo info = from(rloc_0401());
    netdiag_mux_core_dispatch(OT_ERROR_NONE, MSG, &info);
    TEST_ASSERT_EQUAL_INT(0, s_rec[0].calls);
}

TEST_CASE("On a filtered send to a multicast address: answers from any address reach the listener", "[netdiag_mux]")
{
    reset();
    send(multicast(), cb_a, NULL, true);
    otMessageInfo info = from(rloc_0401());
    netdiag_mux_core_dispatch(OT_ERROR_NONE, MSG, &info);
    TEST_ASSERT_EQUAL_INT(1, s_rec[0].calls);
}

TEST_CASE("On a filtered send to an anycast locator: answers from any address reach the listener", "[netdiag_mux]")
{
    reset();
    send(aloc_leader(), cb_a, NULL, true);
    otMessageInfo info = from(rloc_0400());
    netdiag_mux_core_dispatch(OT_ERROR_NONE, MSG, &info);
    TEST_ASSERT_EQUAL_INT(1, s_rec[0].calls);
}

TEST_CASE("On an unfiltered send to a unicast address: answers from any address reach the listener",
          "[netdiag_mux]")
{
    reset();
    send(rloc_0400(), cb_a, NULL, false);
    otMessageInfo info = from(rloc_0401());
    netdiag_mux_core_dispatch(OT_ERROR_NONE, MSG, &info);
    TEST_ASSERT_EQUAL_INT(1, s_rec[0].calls);
}

TEST_CASE("On a later multicast send of a filtered callback: the peer filter is cleared", "[netdiag_mux]")
{
    reset();
    send(rloc_0400(), cb_a, NULL, true);
    send(multicast(), cb_a, NULL, true);
    otMessageInfo info = from(rloc_0401());
    netdiag_mux_core_dispatch(OT_ERROR_NONE, MSG, &info);
    TEST_ASSERT_EQUAL_INT(1, s_rec[0].calls);
}

TEST_CASE("On a later filtered send of a callback: the peer filter is the new dest", "[netdiag_mux]")
{
    reset();
    send(multicast(), cb_a, NULL, true);
    send(rloc_0401(), cb_a, NULL, true);
    otMessageInfo other = from(rloc_0400());
    otMessageInfo dest = from(rloc_0401());
    netdiag_mux_core_dispatch(OT_ERROR_NONE, MSG, &other);
    netdiag_mux_core_dispatch(OT_ERROR_NONE, MSG, &dest);
    TEST_ASSERT_EQUAL_INT(1, s_rec[0].calls);
    TEST_ASSERT_EQUAL_PTR(&dest, s_rec[0].info[0]);
}

TEST_CASE("On a failed ops.send with a new callback: no listener is registered", "[netdiag_mux]")
{
    reset();
    s_send_ret = OT_ERROR_NO_BUFS;
    TEST_ASSERT_EQUAL_INT(OT_ERROR_NO_BUFS, send(rloc_0400(), cb_a, NULL, false));
    otMessageInfo info = from(rloc_0400());
    netdiag_mux_core_dispatch(OT_ERROR_NONE, MSG, &info);
    TEST_ASSERT_EQUAL_INT(0, s_rec[0].calls);
}

TEST_CASE("On a failed ops.send with a known callback: the context and filter stay as they were", "[netdiag_mux]")
{
    reset();
    send(rloc_0400(), cb_a, (void *)1, true);
    s_send_ret = OT_ERROR_BUSY;
    send(multicast(), cb_a, (void *)2, false);
    otMessageInfo other = from(rloc_0401());
    otMessageInfo dest = from(rloc_0400());
    netdiag_mux_core_dispatch(OT_ERROR_NONE, MSG, &other);
    netdiag_mux_core_dispatch(OT_ERROR_NONE, MSG, &dest);
    TEST_ASSERT_EQUAL_INT(1, s_rec[0].calls);
    TEST_ASSERT_EQUAL_PTR((void *)1, s_rec[0].ctx[0]);
}

TEST_CASE("On a ninth distinct callback: returns OT_ERROR_NO_BUFS without calling ops.send", "[netdiag_mux]")
{
    reset();
    otReceiveDiagnosticGetCallback cbs[] = { cb_a, cb_b, cb_c, cb_d, cb_e, cb_f, cb_g, cb_h, cb_i };
    for (int i = 0; i < NETDIAG_MUX_MAX_LISTENERS; i++) {
        TEST_ASSERT_EQUAL_INT(OT_ERROR_NONE, send(rloc_0400(), cbs[i], NULL, false));
    }
    TEST_ASSERT_EQUAL_INT(OT_ERROR_NO_BUFS, send(rloc_0400(), cbs[8], NULL, false));
    TEST_ASSERT_EQUAL_INT(8, s_send_calls);
}

TEST_CASE("On a ninth distinct callback: the eight listeners keep getting answers and it gets none", "[netdiag_mux]")
{
    reset();
    otReceiveDiagnosticGetCallback cbs[] = { cb_a, cb_b, cb_c, cb_d, cb_e, cb_f, cb_g, cb_h, cb_i };
    for (int i = 0; i < 9; i++) {
        send(rloc_0400(), cbs[i], NULL, false);
    }
    otMessageInfo info = from(rloc_0400());
    netdiag_mux_core_dispatch(OT_ERROR_NONE, MSG, &info);
    for (int i = 0; i < 8; i++) {
        TEST_ASSERT_EQUAL_INT(1, s_rec[i].calls);
    }
    TEST_ASSERT_EQUAL_INT(0, s_rec[8].calls);
}

TEST_CASE("On a NULL dest or callback: returns OT_ERROR_INVALID_ARGS without calling ops.send", "[netdiag_mux]")
{
    reset();
    TEST_ASSERT_EQUAL_INT(OT_ERROR_INVALID_ARGS, send(NULL, cb_a, NULL, false));
    TEST_ASSERT_EQUAL_INT(OT_ERROR_INVALID_ARGS, send(rloc_0400(), NULL, NULL, false));
    TEST_ASSERT_EQUAL_INT(0, s_send_calls);
}

// netdiag_mux_core_dispatch

TEST_CASE("On an answer: every matching listener gets OT_ERROR_NONE, the message, the info and its own context",
          "[netdiag_mux]")
{
    reset();
    send(rloc_0400(), cb_a, (void *)1, true);  // matches
    send(multicast(), cb_b, (void *)2, true);  // unfiltered
    send(rloc_0401(), cb_c, (void *)3, true);  // filtered out
    send(rloc_0401(), cb_d, (void *)4, false); // unfiltered
    otMessageInfo info = from(rloc_0400());
    netdiag_mux_core_dispatch(OT_ERROR_NONE, MSG, &info);
    const int expect_calls[] = { 1, 1, 0, 1 };
    for (int i = 0; i < 4; i++) {
        TEST_ASSERT_EQUAL_INT(expect_calls[i], s_rec[i].calls);
        if (expect_calls[i]) {
            TEST_ASSERT_EQUAL_INT(OT_ERROR_NONE, s_rec[i].error[0]);
            TEST_ASSERT_EQUAL_PTR(MSG, s_rec[i].msg[0]);
            TEST_ASSERT_EQUAL_PTR(&info, s_rec[i].info[0]);
            TEST_ASSERT_EQUAL_PTR((void *)(uintptr_t)(i + 1), s_rec[i].ctx[0]);
        }
    }
}

TEST_CASE("On a listener that sends again from inside dispatch: the current answer still reaches the same listeners",
          "[netdiag_mux]")
{
    reset();
    send(rloc_0400(), cb_a, NULL, false);
    send(rloc_0400(), cb_resend, (void *)1, false);
    send(rloc_0400(), cb_b, NULL, false); // after the re-sender
    otMessageInfo info = from(rloc_0400());
    netdiag_mux_core_dispatch(OT_ERROR_NONE, MSG, &info);
    TEST_ASSERT_EQUAL_INT(1, s_rec[0].calls);
    TEST_ASSERT_EQUAL_INT(1, s_rec[4].calls);
    TEST_ASSERT_EQUAL_INT(1, s_rec[1].calls); // a later listener still gets it
    TEST_ASSERT_EQUAL_INT(0, s_rec[8].calls); // the listener registered by the re-send does not get this answer
}

TEST_CASE("On a unicast address with byte 14 == 0xfc whose IID is not the locator form: still filtered",
          "[netdiag_mux]")
{
    reset();
    otIp6Address fc = locator(0x0400);
    fc.mFields.m8[14] = 0xfc; // byte 14 looks like an ALOC, but the IID is ff:fe00 only in bytes 11-13
    fc.mFields.m8[8] = 0x02;  // a non-zero IID start: not the locator form
    send(&fc, cb_a, NULL, true);
    otMessageInfo other = from(rloc_0401());
    netdiag_mux_core_dispatch(OT_ERROR_NONE, MSG, &other);
    TEST_ASSERT_EQUAL_INT(0, s_rec[0].calls);
    otMessageInfo same = from(&fc);
    netdiag_mux_core_dispatch(OT_ERROR_NONE, MSG, &same);
    TEST_ASSERT_EQUAL_INT(1, s_rec[0].calls);
}

TEST_CASE("On an error: no listener is called", "[netdiag_mux]")
{
    reset();
    send(rloc_0400(), cb_a, NULL, false);
    netdiag_mux_core_dispatch(OT_ERROR_RESPONSE_TIMEOUT, NULL, NULL);
    otMessageInfo info = from(rloc_0400());
    netdiag_mux_core_dispatch(OT_ERROR_FAILED, MSG, &info);
    TEST_ASSERT_EQUAL_INT(0, s_rec[0].calls);
}

TEST_CASE("On an answer without a message or info: no listener is called", "[netdiag_mux]")
{
    reset();
    send(rloc_0400(), cb_a, NULL, false);
    otMessageInfo info = from(rloc_0400());
    netdiag_mux_core_dispatch(OT_ERROR_NONE, NULL, &info);
    netdiag_mux_core_dispatch(OT_ERROR_NONE, MSG, NULL);
    TEST_ASSERT_EQUAL_INT(0, s_rec[0].calls);
}

TEST_CASE("On an answer with no listeners: nothing is called", "[netdiag_mux]")
{
    reset();
    otMessageInfo info = from(rloc_0400());
    netdiag_mux_core_dispatch(OT_ERROR_NONE, MSG, &info);
    for (int i = 0; i < 9; i++) {
        TEST_ASSERT_EQUAL_INT(0, s_rec[i].calls);
    }
}

void app_main(void)
{
    UNITY_BEGIN();
    unity_run_all_tests();
    int failures = (int)UNITY_END();
    exit(failures == 0 ? 0 : 1);
}
