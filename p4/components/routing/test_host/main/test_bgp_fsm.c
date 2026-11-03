#include <stdarg.h>
#include <string.h>
#include "unity.h"
#include "bgp_fsm.h"

#define LOCAL_ASN 65001u
#define PEER_ASN 65002u
#define LOCAL_ID 0x0a000005u /* 10.0.0.5 */
#define LOWER_ID 0x0a000001u
#define HIGHER_ID 0x0a000009u
#define HOLD 90
#define NONE UINT32_MAX

static routing_bgp_fsm_t f;
static routing_bgp_actions_t out;

static void fsm_init(uint16_t hold_time)
{
    routing_bgp_fsm_config_t cfg = {
        .local_asn = LOCAL_ASN,
        .peer_asn = PEER_ASN,
        .local_router_id = LOCAL_ID,
        .hold_time = hold_time,
    };

    routing_bgp_fsm_init(&f, &cfg);
}

static void handle(const routing_bgp_event_t *ev, uint32_t now)
{
    memset(&out, 0xaa, sizeof(out));
    routing_bgp_fsm_handle(&f, ev, now, &out);
}

static void simple(routing_bgp_event_type_t type, uint32_t now)
{
    routing_bgp_event_t ev = {.type = type};

    handle(&ev, now);
}

static void connected(bool incoming, uint32_t now)
{
    routing_bgp_event_t ev = {.type = ROUTING_BGP_EV_CONNECTED, .connected = {.incoming = incoming}};

    handle(&ev, now);
}

static void connect_failed(const char *why, uint32_t now)
{
    routing_bgp_event_t ev = {.type = ROUTING_BGP_EV_CONNECT_FAILED, .connect_failed = {.why = why}};

    handle(&ev, now);
}

static void conn_lost(bool incoming, uint32_t now)
{
    routing_bgp_event_t ev = {.type = ROUTING_BGP_EV_CONN_LOST, .conn_lost = {.incoming = incoming}};

    handle(&ev, now);
}

static void open_rx(uint32_t router_id, uint16_t hold_time, bool incoming, uint32_t now)
{
    routing_bgp_event_t ev = {
        .type = ROUTING_BGP_EV_OPEN_RX,
        .open_rx = {
            .open = {.asn = PEER_ASN, .router_id = router_id, .hold_time = hold_time, .ipv6_unicast = true},
            .incoming = incoming,
        },
    };

    handle(&ev, now);
}

static void open_bad(uint8_t code, uint8_t subcode, bool incoming, uint32_t now)
{
    routing_bgp_event_t ev = {
        .type = ROUTING_BGP_EV_OPEN_BAD,
        .open_bad = {.err = {.code = code, .subcode = subcode}, .incoming = incoming},
    };

    handle(&ev, now);
}

static void update_rx(uint32_t nlri, uint32_t now)
{
    routing_bgp_event_t ev = {.type = ROUTING_BGP_EV_UPDATE_RX, .update_rx = {.nlri = nlri}};

    handle(&ev, now);
}

static void notification_rx(uint8_t code, uint8_t subcode, bool incoming, uint32_t now)
{
    routing_bgp_event_t ev = {
        .type = ROUTING_BGP_EV_NOTIFICATION_RX,
        .notification_rx = {.code = code, .subcode = subcode, .incoming = incoming},
    };

    handle(&ev, now);
}

static void stop(uint8_t subcode, uint32_t now)
{
    routing_bgp_event_t ev = {.type = ROUTING_BGP_EV_STOP, .stop = {.subcode = subcode}};

    handle(&ev, now);
}

static void tick(uint32_t now)
{
    simple(ROUTING_BGP_EV_TICK, now);
}

/* Asserts the action list is exactly the `n` types given. */
static void expect_actions(int n, ...)
{
    va_list ap;

    TEST_ASSERT_EQUAL_UINT8(n, out.count);
    va_start(ap, n);
    for (int i = 0; i < n; i++) {
        TEST_ASSERT_EQUAL_MESSAGE(va_arg(ap, int), out.items[i].type, "action type");
    }
    va_end(ap);
}

static void expect_notification(int index, uint8_t code, uint8_t subcode)
{
    TEST_ASSERT_EQUAL_UINT8(code, out.items[index].notification.code);
    TEST_ASSERT_EQUAL_UINT8(subcode, out.items[index].notification.subcode);
}

static void expect_state(routing_bgp_state_t state)
{
    TEST_ASSERT_EQUAL(state, routing_bgp_fsm_state(&f));
}

static void expect_deadline(uint32_t deadline)
{
    TEST_ASSERT_EQUAL_UINT32(deadline, routing_bgp_fsm_next_deadline_ms(&f));
}

/* START at `now`, outgoing connection up, peer OPEN with `peer_hold`: OpenConfirm. */
static void to_openconfirm(uint16_t peer_hold, uint32_t now)
{
    simple(ROUTING_BGP_EV_START, now);
    connected(false, now);
    open_rx(LOWER_ID, peer_hold, false, now);
    expect_state(ROUTING_BGP_OPENCONFIRM);
}

static void to_established(uint16_t peer_hold, uint32_t now)
{
    to_openconfirm(peer_hold, now);
    simple(ROUTING_BGP_EV_KEEPALIVE_RX, now);
    expect_state(ROUTING_BGP_ESTABLISHED);
}

/* One failed attempt: the retry deadline passes, CONNECT, the connection is refused. */
static void fail_attempt(uint32_t *now)
{
    *now = routing_bgp_fsm_next_deadline_ms(&f);
    tick(*now);
    expect_actions(1, ROUTING_BGP_ACT_CONNECT);
    connect_failed("refused", *now);
}

/* --- start --- */

TEST_CASE("Peer enabled: moves to Connect and asks for an outgoing connection", "[routing]")
{
    fsm_init(HOLD);
    expect_state(ROUTING_BGP_IDLE);
    expect_deadline(NONE);
    simple(ROUTING_BGP_EV_START, 1000);
    expect_state(ROUTING_BGP_CONNECT);
    expect_actions(1, ROUTING_BGP_ACT_CONNECT);
    expect_deadline(1000 + 10000);
}

/* --- connected --- */

TEST_CASE("Outgoing or incoming connection succeeds: sends OPEN and moves to OpenSent", "[routing]")
{
    for (int incoming = 0; incoming <= 1; incoming++) {
        fsm_init(HOLD);
        simple(ROUTING_BGP_EV_START, 0);
        connected(incoming, 500);
        expect_state(ROUTING_BGP_OPENSENT);
        expect_actions(1, ROUTING_BGP_ACT_SEND_OPEN);
        expect_deadline(500 + 240000);
    }
}

/* --- connect failed --- */

TEST_CASE("Connection refused or timed out: moves to Active and schedules a retry after the current backoff",
          "[routing]")
{
    fsm_init(HOLD);
    simple(ROUTING_BGP_EV_START, 0);
    connect_failed("refused", 2000);
    expect_state(ROUTING_BGP_ACTIVE);
    expect_actions(1, ROUTING_BGP_ACT_CLOSE);
    expect_deadline(2000 + 5000);
    TEST_ASSERT_EQUAL_STRING("connect: refused", routing_bgp_fsm_last_error(&f));

    tick(6999);
    expect_actions(0);
    expect_state(ROUTING_BGP_ACTIVE);
    tick(7000);
    expect_actions(1, ROUTING_BGP_ACT_CONNECT);
    expect_state(ROUTING_BGP_CONNECT);
    expect_deadline(7000 + 10000);
}

TEST_CASE("Connection refused or timed out: doubles the backoff, up to 120 s", "[routing]")
{
    static const uint32_t expected[] = {5000, 10000, 20000, 40000, 80000, 120000, 120000};
    uint32_t now = 0;

    fsm_init(HOLD);
    simple(ROUTING_BGP_EV_START, now);
    connect_failed("refused", now);
    for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); i++) {
        TEST_ASSERT_EQUAL_UINT32(expected[i], routing_bgp_fsm_next_deadline_ms(&f) - now);
        fail_attempt(&now);
    }
}

/* --- connect timeout --- */

TEST_CASE("No connection after 10 s: same effects as connect failed", "[routing]")
{
    fsm_init(HOLD);
    simple(ROUTING_BGP_EV_START, 1000);
    tick(10999);
    expect_actions(0);
    expect_state(ROUTING_BGP_CONNECT);
    tick(11000);
    expect_state(ROUTING_BGP_ACTIVE);
    expect_actions(1, ROUTING_BGP_ACT_CLOSE);
    expect_deadline(11000 + 5000);
    TEST_ASSERT_EQUAL_STRING("connect: timed out", routing_bgp_fsm_last_error(&f));

    /* the backoff doubled, as after a refused connection */
    tick(16000);
    tick(26000);
    expect_deadline(26000 + 10000);
}

/* --- open received --- */

TEST_CASE("Valid OPEN in OpenSent: sends KEEPALIVE, moves to OpenConfirm, negotiates hold time as the smaller of "
          "the two", "[routing]")
{
    fsm_init(HOLD);
    simple(ROUTING_BGP_EV_START, 0);
    connected(false, 0);
    open_rx(LOWER_ID, 180, false, 1000);
    expect_actions(1, ROUTING_BGP_ACT_SEND_KEEPALIVE);
    expect_state(ROUTING_BGP_OPENCONFIRM);
    TEST_ASSERT_EQUAL_UINT16(90, routing_bgp_fsm_hold_time(&f));
    expect_deadline(1000 + 30000); /* keepalive every 90 / 3 s */

    fsm_init(HOLD);
    simple(ROUTING_BGP_EV_START, 0);
    connected(false, 0);
    open_rx(LOWER_ID, 30, false, 1000);
    TEST_ASSERT_EQUAL_UINT16(30, routing_bgp_fsm_hold_time(&f));
    expect_deadline(1000 + 10000);
}

TEST_CASE("Invalid OPEN: sends the matching NOTIFICATION, closes, moves to Idle and schedules a retry", "[routing]")
{
    fsm_init(HOLD);
    simple(ROUTING_BGP_EV_START, 0);
    connected(false, 0);
    open_bad(2, 7, false, 1000);
    expect_actions(2, ROUTING_BGP_ACT_SEND_NOTIFICATION, ROUTING_BGP_ACT_CLOSE);
    expect_notification(0, 2, 7);
    expect_state(ROUTING_BGP_IDLE);
    expect_deadline(1000 + 5000);
    TEST_ASSERT_EQUAL_STRING("notification 2/7 sent", routing_bgp_fsm_last_error(&f));
    tick(6000);
    expect_actions(1, ROUTING_BGP_ACT_CONNECT);
    expect_state(ROUTING_BGP_CONNECT);
}

TEST_CASE("Peer offers hold time 0, own 90: negotiates 0; no keepalive deadline and no hold deadline afterwards",
          "[routing]")
{
    fsm_init(HOLD);
    to_openconfirm(0, 0);
    TEST_ASSERT_EQUAL_UINT16(0, routing_bgp_fsm_hold_time(&f));
    expect_deadline(240000); /* only the handshake guard */
    simple(ROUTING_BGP_EV_KEEPALIVE_RX, 1000);
    expect_state(ROUTING_BGP_ESTABLISHED);
    expect_deadline(NONE);

    tick(1000 + 3600000);
    expect_actions(0);
    expect_state(ROUTING_BGP_ESTABLISHED);
    update_rx(1, 2000 + 3600000);
    expect_deadline(NONE);
}

/* --- keepalive received --- */

TEST_CASE("In OpenConfirm: moves to Established, resets the backoff, sends the full announcement set", "[routing]")
{
    uint32_t now = 0;

    fsm_init(HOLD);
    simple(ROUTING_BGP_EV_START, now);
    connect_failed("refused", now);
    fail_attempt(&now);
    fail_attempt(&now); /* backoff now 40 s */
    now = routing_bgp_fsm_next_deadline_ms(&f);
    tick(now);
    connected(false, now);
    open_rx(LOWER_ID, HOLD, false, now);
    simple(ROUTING_BGP_EV_KEEPALIVE_RX, now);
    expect_state(ROUTING_BGP_ESTABLISHED);
    expect_actions(1, ROUTING_BGP_ACT_SEND_FULL_SET);

    /* backoff reset: a failed reconnect after this session waits 5 s, then 10 s */
    conn_lost(false, now + 1000);
    TEST_ASSERT_EQUAL_UINT32(5000, routing_bgp_fsm_next_deadline_ms(&f) - (now + 1000));
    now = routing_bgp_fsm_next_deadline_ms(&f);
    tick(now);
    connect_failed("refused", now);
    TEST_ASSERT_EQUAL_UINT32(10000, routing_bgp_fsm_next_deadline_ms(&f) - now);
}

TEST_CASE("In Established: restarts the hold timer", "[routing]")
{
    fsm_init(HOLD);
    to_established(HOLD, 0);
    /* keepalives sent on schedule so that only the hold timer can end the session */
    tick(30000);
    expect_actions(1, ROUTING_BGP_ACT_SEND_KEEPALIVE);
    simple(ROUTING_BGP_EV_KEEPALIVE_RX, 50000);
    expect_actions(0);
    tick(60000);
    tick(90000);
    expect_actions(1, ROUTING_BGP_ACT_SEND_KEEPALIVE);
    expect_state(ROUTING_BGP_ESTABLISHED);
    tick(120000);
    expect_actions(1, ROUTING_BGP_ACT_SEND_KEEPALIVE);
    expect_state(ROUTING_BGP_ESTABLISHED);
    tick(140000);
    expect_state(ROUTING_BGP_IDLE);
}

TEST_CASE("Keepalive deadline passes in Established: sends KEEPALIVE and arms the next one", "[routing]")
{
    fsm_init(HOLD);
    to_established(HOLD, 0);
    expect_deadline(30000);
    tick(29999);
    expect_actions(0);
    tick(30000);
    expect_actions(1, ROUTING_BGP_ACT_SEND_KEEPALIVE);
    expect_deadline(60000);
}

/* --- update received --- */

TEST_CASE("In Established: increments received_dropped by the number of announced NLRI and restarts the hold timer",
          "[routing]")
{
    fsm_init(HOLD);
    to_established(HOLD, 0);
    update_rx(3, 1000);
    update_rx(2, 2000);
    TEST_ASSERT_EQUAL_UINT32(5, routing_bgp_fsm_received_dropped(&f));
    tick(30000); /* keepalive */
    tick(60000); /* keepalive */
    tick(90000); /* would have expired without the UPDATE at 2000 */
    expect_state(ROUTING_BGP_ESTABLISHED);
    tick(92000);
    expect_state(ROUTING_BGP_IDLE);
}

TEST_CASE("In Established: installs nothing", "[routing]")
{
    fsm_init(HOLD);
    to_established(HOLD, 0);
    update_rx(4, 1000);
    expect_actions(0);
}

TEST_CASE("Peer reconnects: received_dropped is kept over the peer's life", "[routing]")
{
    fsm_init(HOLD);
    to_established(HOLD, 0);
    update_rx(3, 1000);
    conn_lost(false, 2000);
    tick(7000);
    connected(false, 7000);
    open_rx(LOWER_ID, HOLD, false, 7000);
    simple(ROUTING_BGP_EV_KEEPALIVE_RX, 7000);
    update_rx(2, 8000);
    TEST_ASSERT_EQUAL_UINT32(5, routing_bgp_fsm_received_dropped(&f));
}

/* --- unexpected messages --- */

TEST_CASE("Message not allowed in the state: sends NOTIFICATION 5/<state>, closes, moves to Idle", "[routing]")
{
    fsm_init(HOLD);
    simple(ROUTING_BGP_EV_START, 0);
    connected(false, 0);
    simple(ROUTING_BGP_EV_KEEPALIVE_RX, 100);
    expect_actions(2, ROUTING_BGP_ACT_SEND_NOTIFICATION, ROUTING_BGP_ACT_CLOSE);
    expect_notification(0, 5, 1);
    expect_state(ROUTING_BGP_IDLE);
    TEST_ASSERT_EQUAL_STRING("notification 5/1 sent", routing_bgp_fsm_last_error(&f));

    fsm_init(HOLD);
    to_openconfirm(HOLD, 0);
    update_rx(1, 100);
    expect_notification(0, 5, 2);
    expect_state(ROUTING_BGP_IDLE);

    fsm_init(HOLD);
    to_established(HOLD, 0);
    open_rx(LOWER_ID, HOLD, false, 100);
    expect_actions(2, ROUTING_BGP_ACT_SEND_NOTIFICATION, ROUTING_BGP_ACT_CLOSE);
    expect_notification(0, 5, 3);
    expect_state(ROUTING_BGP_IDLE);
}

/* --- hold timer expires --- */

TEST_CASE("Negotiated hold time is not 0: sends NOTIFICATION 4/0, closes, moves to Idle, sets last_error \"hold timer "
          "expired\"", "[routing]")
{
    fsm_init(HOLD);
    to_established(HOLD, 0);
    tick(30000);
    tick(60000);
    tick(89999);
    expect_state(ROUTING_BGP_ESTABLISHED);
    tick(90000);
    expect_actions(2, ROUTING_BGP_ACT_SEND_NOTIFICATION, ROUTING_BGP_ACT_CLOSE);
    expect_notification(0, 4, 0);
    expect_state(ROUTING_BGP_IDLE);
    TEST_ASSERT_EQUAL_STRING("hold timer expired", routing_bgp_fsm_last_error(&f));
    expect_deadline(90000 + 5000);
}

TEST_CASE("No OPEN within 240 s in OpenSent: sends NOTIFICATION 4/0, closes, moves to Idle", "[routing]")
{
    fsm_init(HOLD);
    simple(ROUTING_BGP_EV_START, 0);
    connected(false, 0);
    tick(239999);
    expect_state(ROUTING_BGP_OPENSENT);
    tick(240000);
    expect_actions(2, ROUTING_BGP_ACT_SEND_NOTIFICATION, ROUTING_BGP_ACT_CLOSE);
    expect_notification(0, 4, 0);
    expect_state(ROUTING_BGP_IDLE);
    TEST_ASSERT_EQUAL_STRING("hold timer expired", routing_bgp_fsm_last_error(&f));
}

/* --- notification received --- */

TEST_CASE("Any state: closes, moves to Idle, records the code and subcode in last_error, schedules a retry",
          "[routing]")
{
    for (int s = 0; s < 3; s++) {
        fsm_init(HOLD);
        simple(ROUTING_BGP_EV_START, 0);
        connected(false, 0);
        if (s >= 1) {
            open_rx(LOWER_ID, HOLD, false, 0);
        }
        if (s >= 2) {
            simple(ROUTING_BGP_EV_KEEPALIVE_RX, 0);
        }
        notification_rx(6, 4, false, 1000);
        expect_actions(1, ROUTING_BGP_ACT_CLOSE);
        expect_state(ROUTING_BGP_IDLE);
        TEST_ASSERT_EQUAL_STRING("notification 6/4 received", routing_bgp_fsm_last_error(&f));
        expect_deadline(1000 + 5000);
    }
}

/* --- connection lost --- */

TEST_CASE("TCP reset in Established after the backoff had grown to 40 s earlier: moves to Idle, records last_error, "
          "schedules a retry after 5 s", "[routing]")
{
    uint32_t now = 0;

    fsm_init(HOLD);
    simple(ROUTING_BGP_EV_START, now);
    connect_failed("refused", now);
    fail_attempt(&now);
    fail_attempt(&now);
    fail_attempt(&now);
    TEST_ASSERT_EQUAL_UINT32(40000, routing_bgp_fsm_next_deadline_ms(&f) - now);
    now = routing_bgp_fsm_next_deadline_ms(&f);
    tick(now);
    connected(false, now);
    open_rx(LOWER_ID, HOLD, false, now);
    simple(ROUTING_BGP_EV_KEEPALIVE_RX, now);

    conn_lost(false, now + 60000);
    expect_actions(1, ROUTING_BGP_ACT_CLOSE);
    expect_state(ROUTING_BGP_IDLE);
    TEST_ASSERT_EQUAL_STRING("connection lost", routing_bgp_fsm_last_error(&f));
    expect_deadline(now + 60000 + 5000);
}

TEST_CASE("Connection lost before Established: moves to Idle and retries after the grown backoff", "[routing]")
{
    fsm_init(HOLD);
    simple(ROUTING_BGP_EV_START, 0);
    connected(false, 0);
    conn_lost(false, 1000);
    expect_state(ROUTING_BGP_IDLE);
    expect_deadline(1000 + 5000);
    tick(6000);
    connected(false, 6000);
    conn_lost(false, 7000);
    expect_deadline(7000 + 10000);
}

/* --- uptime and counters --- */

TEST_CASE("Established for 125 s: uptime_s is 125", "[routing]")
{
    fsm_init(0);
    TEST_ASSERT_EQUAL_UINT32(0, routing_bgp_fsm_uptime_s(&f, 5000));
    to_openconfirm(HOLD, 1000);
    TEST_ASSERT_EQUAL_UINT32(0, routing_bgp_fsm_uptime_s(&f, 5000));
    simple(ROUTING_BGP_EV_KEEPALIVE_RX, 10000);
    TEST_ASSERT_EQUAL_UINT32(125, routing_bgp_fsm_uptime_s(&f, 135000));
    conn_lost(false, 140000);
    TEST_ASSERT_EQUAL_UINT32(0, routing_bgp_fsm_uptime_s(&f, 141000));
}

TEST_CASE("Established past the 2^32 ms clock wrap: uptime_s keeps counting", "[routing]")
{
    const uint32_t step = 0x80000000u; /* 2^31 ms, folded each step */

    fsm_init(0);
    to_openconfirm(0, 1000); /* hold time 0: no timer, so only the clock calls fold */
    simple(ROUTING_BGP_EV_KEEPALIVE_RX, 10000);
    routing_bgp_fsm_clock(&f, 10000 + step);
    routing_bgp_fsm_clock(&f, 10000 + 2 * step);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)(3ull * step / 1000), routing_bgp_fsm_uptime_s(&f, 10000 + 3 * step));
}

TEST_CASE("Established for 125 s: resets counts every drop from Established", "[routing]")
{
    uint32_t now = 0;

    fsm_init(HOLD);
    to_established(HOLD, now);
    TEST_ASSERT_EQUAL_UINT32(0, routing_bgp_fsm_resets(&f));

    conn_lost(false, now += 1000);
    TEST_ASSERT_EQUAL_UINT32(1, routing_bgp_fsm_resets(&f));

    /* a drop before Established is not counted */
    now = routing_bgp_fsm_next_deadline_ms(&f);
    tick(now);
    connected(false, now);
    notification_rx(6, 4, false, now);
    TEST_ASSERT_EQUAL_UINT32(1, routing_bgp_fsm_resets(&f));

    now = routing_bgp_fsm_next_deadline_ms(&f);
    tick(now);
    connected(false, now);
    open_rx(LOWER_ID, HOLD, false, now);
    simple(ROUTING_BGP_EV_KEEPALIVE_RX, now);
    notification_rx(6, 4, false, now + 1000);
    TEST_ASSERT_EQUAL_UINT32(2, routing_bgp_fsm_resets(&f));

    now = routing_bgp_fsm_next_deadline_ms(&f);
    tick(now);
    connected(false, now);
    open_rx(LOWER_ID, HOLD, false, now);
    simple(ROUTING_BGP_EV_KEEPALIVE_RX, now);
    tick(now + 30000);
    tick(now + 60000);
    tick(now + 90000); /* hold expiry */
    TEST_ASSERT_EQUAL_UINT32(3, routing_bgp_fsm_resets(&f));

    /* a stop is not a drop */
    now = routing_bgp_fsm_next_deadline_ms(&f);
    tick(now);
    connected(false, now);
    open_rx(LOWER_ID, HOLD, false, now);
    simple(ROUTING_BGP_EV_KEEPALIVE_RX, now);
    stop(ROUTING_BGP_CEASE_ADMIN_SHUTDOWN, now + 1000);
    TEST_ASSERT_EQUAL_UINT32(3, routing_bgp_fsm_resets(&f));
}

/* --- collision --- */

TEST_CASE("Incoming connection while an outgoing one is in OpenSent or OpenConfirm: keeps the connection opened by "
          "the side with the higher router ID and closes the other with NOTIFICATION 6/7", "[routing]")
{
    for (int in_confirm = 0; in_confirm <= 1; in_confirm++) {
        /* local ID higher: the outgoing connection (ours) stays */
        fsm_init(HOLD);
        simple(ROUTING_BGP_EV_START, 0);
        connected(false, 0);
        if (in_confirm) {
            open_rx(LOWER_ID, HOLD, false, 0);
        }
        simple(ROUTING_BGP_EV_INCOMING_WHILE_BUSY, 100);
        expect_actions(0);
        open_rx(LOWER_ID, HOLD, true, 200);
        expect_actions(1, ROUTING_BGP_ACT_REJECT_INCOMING);
        expect_notification(0, 6, 7);
        expect_state(in_confirm ? ROUTING_BGP_OPENCONFIRM : ROUTING_BGP_OPENSENT);
        /* the current connection carries on */
        if (!in_confirm) {
            open_rx(LOWER_ID, HOLD, false, 300);
            expect_actions(1, ROUTING_BGP_ACT_SEND_KEEPALIVE);
            expect_state(ROUTING_BGP_OPENCONFIRM);
        }

        /* peer ID higher: the incoming connection (the peer's) is adopted */
        fsm_init(HOLD);
        simple(ROUTING_BGP_EV_START, 0);
        connected(false, 0);
        if (in_confirm) {
            open_rx(HIGHER_ID, HOLD, false, 0);
        }
        simple(ROUTING_BGP_EV_INCOMING_WHILE_BUSY, 100);
        expect_actions(0);
        open_rx(HIGHER_ID, 60, true, 200);
        expect_actions(4, ROUTING_BGP_ACT_SEND_NOTIFICATION, ROUTING_BGP_ACT_ADOPT_INCOMING,
                       ROUTING_BGP_ACT_SEND_OPEN, ROUTING_BGP_ACT_SEND_KEEPALIVE);
        expect_notification(0, 6, 7);
        expect_state(ROUTING_BGP_OPENCONFIRM);
        TEST_ASSERT_EQUAL_UINT16(60, routing_bgp_fsm_hold_time(&f));
        expect_deadline(200 + 20000);
    }
}

TEST_CASE("Incoming connection in Connect or Active: adopts it, sends OPEN and moves to OpenSent", "[routing]")
{
    fsm_init(HOLD);
    simple(ROUTING_BGP_EV_START, 0);
    simple(ROUTING_BGP_EV_INCOMING_WHILE_BUSY, 100);
    expect_actions(2, ROUTING_BGP_ACT_ADOPT_INCOMING, ROUTING_BGP_ACT_SEND_OPEN);
    expect_state(ROUTING_BGP_OPENSENT);
    expect_deadline(100 + 240000);

    fsm_init(HOLD);
    simple(ROUTING_BGP_EV_START, 0);
    connect_failed("refused", 0);
    simple(ROUTING_BGP_EV_INCOMING_WHILE_BUSY, 100);
    expect_actions(2, ROUTING_BGP_ACT_ADOPT_INCOMING, ROUTING_BGP_ACT_SEND_OPEN);
    expect_state(ROUTING_BGP_OPENSENT);
    expect_deadline(100 + 240000);
}

TEST_CASE("Incoming connection in Established: rejects it with NOTIFICATION 6/7", "[routing]")
{
    fsm_init(HOLD);
    to_established(HOLD, 0);
    simple(ROUTING_BGP_EV_INCOMING_WHILE_BUSY, 100);
    expect_actions(1, ROUTING_BGP_ACT_REJECT_INCOMING);
    expect_notification(0, 6, 7);
    expect_state(ROUTING_BGP_ESTABLISHED);
}

TEST_CASE("OpenConfirm reaches Established while an incoming connection is pending: rejects it with NOTIFICATION "
          "6/7", "[routing]")
{
    fsm_init(HOLD);
    to_openconfirm(HOLD, 0);
    simple(ROUTING_BGP_EV_INCOMING_WHILE_BUSY, 100);
    simple(ROUTING_BGP_EV_KEEPALIVE_RX, 200);
    expect_actions(2, ROUTING_BGP_ACT_SEND_FULL_SET, ROUTING_BGP_ACT_REJECT_INCOMING);
    expect_notification(1, 6, 7);
    expect_state(ROUTING_BGP_ESTABLISHED);
}

TEST_CASE("Current connection drops while an incoming one is pending: adopts the incoming one, sends OPEN, moves to "
          "OpenSent", "[routing]")
{
    fsm_init(HOLD);
    simple(ROUTING_BGP_EV_START, 0);
    connected(false, 0);
    simple(ROUTING_BGP_EV_INCOMING_WHILE_BUSY, 100);
    notification_rx(6, 7, false, 200); /* the peer resolved the collision against our connection */
    expect_actions(2, ROUTING_BGP_ACT_ADOPT_INCOMING, ROUTING_BGP_ACT_SEND_OPEN);
    expect_state(ROUTING_BGP_OPENSENT);
    expect_deadline(200 + 240000);
    TEST_ASSERT_EQUAL_STRING("notification 6/7 received", routing_bgp_fsm_last_error(&f));

    /* the adopted connection is now the current one: its OPEN comes tagged false */
    open_rx(HIGHER_ID, HOLD, false, 300);
    expect_actions(1, ROUTING_BGP_ACT_SEND_KEEPALIVE);
    expect_state(ROUTING_BGP_OPENCONFIRM);

    fsm_init(HOLD);
    simple(ROUTING_BGP_EV_START, 0);
    connected(false, 0);
    simple(ROUTING_BGP_EV_INCOMING_WHILE_BUSY, 100);
    tick(240000);
    expect_actions(3, ROUTING_BGP_ACT_SEND_NOTIFICATION, ROUTING_BGP_ACT_ADOPT_INCOMING,
                   ROUTING_BGP_ACT_SEND_OPEN);
    expect_notification(0, 4, 0);
    expect_state(ROUTING_BGP_OPENSENT);
}

TEST_CASE("Pending incoming connection fails: closes it and keeps the current one", "[routing]")
{
    /* bad OPEN: its NOTIFICATION goes out on the incoming connection */
    fsm_init(HOLD);
    simple(ROUTING_BGP_EV_START, 0);
    connected(false, 0);
    simple(ROUTING_BGP_EV_INCOMING_WHILE_BUSY, 100);
    open_bad(2, 2, true, 200);
    expect_actions(1, ROUTING_BGP_ACT_REJECT_INCOMING);
    expect_notification(0, 2, 2);
    expect_state(ROUTING_BGP_OPENSENT);

    /* lost or notified: closed without a NOTIFICATION (code 0) */
    fsm_init(HOLD);
    simple(ROUTING_BGP_EV_START, 0);
    connected(false, 0);
    simple(ROUTING_BGP_EV_INCOMING_WHILE_BUSY, 100);
    conn_lost(true, 200);
    expect_actions(1, ROUTING_BGP_ACT_REJECT_INCOMING);
    expect_notification(0, 0, 0);
    expect_state(ROUTING_BGP_OPENSENT);

    fsm_init(HOLD);
    to_openconfirm(HOLD, 0);
    simple(ROUTING_BGP_EV_INCOMING_WHILE_BUSY, 100);
    notification_rx(6, 7, true, 200);
    expect_actions(1, ROUTING_BGP_ACT_REJECT_INCOMING);
    expect_notification(0, 0, 0);
    expect_state(ROUTING_BGP_OPENCONFIRM);
    TEST_ASSERT_EQUAL_STRING("", routing_bgp_fsm_last_error(&f));

    /* nothing pending any more: a drop of the current connection goes to Idle */
    conn_lost(false, 300);
    expect_actions(1, ROUTING_BGP_ACT_CLOSE);
    expect_state(ROUTING_BGP_IDLE);
}

TEST_CASE("Incoming connection in Idle: accepted while started, refused with NOTIFICATION 6/5 when stopped",
          "[routing]")
{
    fsm_init(HOLD);
    to_established(HOLD, 0);
    conn_lost(false, 1000); /* Idle, retry at 6000 */
    connected(true, 2000);
    expect_actions(1, ROUTING_BGP_ACT_SEND_OPEN);
    expect_state(ROUTING_BGP_OPENSENT);
    expect_deadline(2000 + 240000);

    fsm_init(HOLD);
    connected(true, 0);
    expect_actions(1, ROUTING_BGP_ACT_CLOSE);
    expect_state(ROUTING_BGP_IDLE);
    simple(ROUTING_BGP_EV_INCOMING_WHILE_BUSY, 0);
    expect_actions(1, ROUTING_BGP_ACT_REJECT_INCOMING);
    expect_notification(0, 6, 5);
    expect_state(ROUTING_BGP_IDLE);
}

/* --- announcement set changed --- */

TEST_CASE("In Established: sends one UPDATE withdrawing removed prefixes and one announcing added prefixes",
          "[routing]")
{
    fsm_init(HOLD);
    to_established(HOLD, 0);
    simple(ROUTING_BGP_EV_ANNOUNCE_CHANGED, 1000);
    expect_actions(1, ROUTING_BGP_ACT_SEND_DIFF);
}

TEST_CASE("Not established: sends nothing; the full set goes out on the next Established", "[routing]")
{
    fsm_init(HOLD);
    simple(ROUTING_BGP_EV_ANNOUNCE_CHANGED, 0);
    expect_actions(0);
    simple(ROUTING_BGP_EV_START, 0);
    simple(ROUTING_BGP_EV_ANNOUNCE_CHANGED, 0);
    expect_actions(0);
    connected(false, 0);
    simple(ROUTING_BGP_EV_ANNOUNCE_CHANGED, 0);
    expect_actions(0);
    open_rx(LOWER_ID, HOLD, false, 0);
    simple(ROUTING_BGP_EV_ANNOUNCE_CHANGED, 0);
    expect_actions(0);
    simple(ROUTING_BGP_EV_KEEPALIVE_RX, 0);
    expect_actions(1, ROUTING_BGP_ACT_SEND_FULL_SET);
}

/* --- stop --- */

TEST_CASE("Method changed away from bgp, or peer removed: sends NOTIFICATION 6/2 (or 6/3 for a removed peer) when "
          "connected, closes, stays in Idle", "[routing]")
{
    static const uint8_t subcodes[] = {ROUTING_BGP_CEASE_ADMIN_SHUTDOWN, ROUTING_BGP_CEASE_PEER_DECONFIGURED};

    for (size_t i = 0; i < sizeof(subcodes); i++) {
        /* connected: OpenSent, OpenConfirm, Established */
        for (int s = 0; s < 3; s++) {
            fsm_init(HOLD);
            simple(ROUTING_BGP_EV_START, 0);
            connected(false, 0);
            if (s >= 1) {
                open_rx(LOWER_ID, HOLD, false, 0);
            }
            if (s >= 2) {
                simple(ROUTING_BGP_EV_KEEPALIVE_RX, 0);
            }
            stop(subcodes[i], 1000);
            expect_actions(2, ROUTING_BGP_ACT_SEND_NOTIFICATION, ROUTING_BGP_ACT_CLOSE);
            expect_notification(0, 6, subcodes[i]);
            expect_state(ROUTING_BGP_IDLE);
            expect_deadline(NONE);
            tick(1000 + 600000);
            expect_actions(0);
            expect_state(ROUTING_BGP_IDLE);
        }
    }

    /* not connected: Connect, Active */
    fsm_init(HOLD);
    simple(ROUTING_BGP_EV_START, 0);
    stop(ROUTING_BGP_CEASE_ADMIN_SHUTDOWN, 1000);
    expect_actions(1, ROUTING_BGP_ACT_CLOSE);
    expect_state(ROUTING_BGP_IDLE);
    expect_deadline(NONE);

    fsm_init(HOLD);
    simple(ROUTING_BGP_EV_START, 0);
    connect_failed("refused", 0);
    stop(ROUTING_BGP_CEASE_ADMIN_SHUTDOWN, 1000);
    expect_actions(1, ROUTING_BGP_ACT_CLOSE);
    expect_state(ROUTING_BGP_IDLE);
    expect_deadline(NONE);
    tick(600000);
    expect_actions(0);

    /* Idle waiting for a retry: the retry is cancelled */
    fsm_init(HOLD);
    to_established(HOLD, 0);
    conn_lost(false, 1000);
    stop(ROUTING_BGP_CEASE_ADMIN_SHUTDOWN, 2000);
    expect_actions(0);
    expect_deadline(NONE);

    /* with a pending incoming connection: it gets the same NOTIFICATION */
    fsm_init(HOLD);
    simple(ROUTING_BGP_EV_START, 0);
    connected(false, 0);
    simple(ROUTING_BGP_EV_INCOMING_WHILE_BUSY, 100);
    stop(ROUTING_BGP_CEASE_PEER_DECONFIGURED, 200);
    expect_actions(3, ROUTING_BGP_ACT_SEND_NOTIFICATION, ROUTING_BGP_ACT_CLOSE, ROUTING_BGP_ACT_REJECT_INCOMING);
    expect_notification(2, 6, 3);

    /* a new START begins again */
    simple(ROUTING_BGP_EV_START, 300);
    expect_actions(1, ROUTING_BGP_ACT_CONNECT);
    expect_state(ROUTING_BGP_CONNECT);
}

TEST_CASE("Clock wraps past UINT32_MAX: timers still fire on time", "[routing]")
{
    const uint32_t start = UINT32_MAX - 1000;

    fsm_init(HOLD);
    to_established(HOLD, start);
    expect_deadline(start + 30000);
    tick(start + 29999);
    expect_actions(0);
    tick(start + 30000);
    expect_actions(1, ROUTING_BGP_ACT_SEND_KEEPALIVE);
    TEST_ASSERT_EQUAL_UINT32(40, routing_bgp_fsm_uptime_s(&f, start + 40000));
}

/* --- fix round 1 --- */

TEST_CASE("Event tagged incoming with nothing pending: leaves the current session untouched", "[routing]")
{
    for (int established = 0; established <= 1; established++) {
        fsm_init(HOLD);
        simple(ROUTING_BGP_EV_START, 0);
        connected(false, 0);
        if (established) {
            open_rx(LOWER_ID, HOLD, false, 0);
            simple(ROUTING_BGP_EV_KEEPALIVE_RX, 0);
            simple(ROUTING_BGP_EV_INCOMING_WHILE_BUSY, 100);
        } else {
            simple(ROUTING_BGP_EV_INCOMING_WHILE_BUSY, 100);
            open_rx(LOWER_ID, HOLD, true, 100); /* local ID higher */
        }
        expect_actions(1, ROUTING_BGP_ACT_REJECT_INCOMING);
        routing_bgp_state_t state = routing_bgp_fsm_state(&f);
        uint32_t deadline = routing_bgp_fsm_next_deadline_ms(&f);

        /* the rejected connection's NOTIFICATION, FIN and a late OPEN */
        notification_rx(6, 7, true, 200);
        expect_actions(0);
        conn_lost(true, 300);
        expect_actions(0);
        open_rx(HIGHER_ID, HOLD, true, 400);
        expect_actions(0);
        open_bad(2, 2, true, 500);
        expect_actions(0);
        expect_state(state);
        expect_deadline(deadline);
        TEST_ASSERT_EQUAL_STRING("", routing_bgp_fsm_last_error(&f));
        TEST_ASSERT_EQUAL_UINT32(0, routing_bgp_fsm_resets(&f));
    }
}

TEST_CASE("Negotiated hold time 0 and no KEEPALIVE in OpenConfirm for 240 s: sends NOTIFICATION 4/0, closes, moves "
          "to Idle", "[routing]")
{
    fsm_init(HOLD);
    to_openconfirm(0, 1000);
    tick(1000 + 239999);
    expect_actions(0);
    expect_state(ROUTING_BGP_OPENCONFIRM);
    tick(1000 + 240000);
    expect_actions(2, ROUTING_BGP_ACT_SEND_NOTIFICATION, ROUTING_BGP_ACT_CLOSE);
    expect_notification(0, 4, 0);
    expect_state(ROUTING_BGP_IDLE);
    TEST_ASSERT_EQUAL_STRING("hold timer expired", routing_bgp_fsm_last_error(&f));
}

TEST_CASE("Timer that would end at UINT32_MAX: ends 1 ms later at 0, never reported as none", "[routing]")
{
    fsm_init(HOLD);
    simple(ROUTING_BGP_EV_START, UINT32_MAX - ROUTING_BGP_CONNECT_TIMEOUT_MS);
    expect_deadline(0);
    tick(UINT32_MAX);
    expect_actions(0);
    expect_state(ROUTING_BGP_CONNECT);
    tick(0);
    expect_actions(1, ROUTING_BGP_ACT_CLOSE);
    expect_state(ROUTING_BGP_ACTIVE);
}

TEST_CASE("New incoming connection while the current one is incoming: replaces it and sends OPEN", "[routing]")
{
    /* adopted in Connect, replaced in OpenSent */
    fsm_init(HOLD);
    simple(ROUTING_BGP_EV_START, 0);
    simple(ROUTING_BGP_EV_INCOMING_WHILE_BUSY, 100);
    simple(ROUTING_BGP_EV_INCOMING_WHILE_BUSY, 200);
    expect_actions(2, ROUTING_BGP_ACT_ADOPT_INCOMING, ROUTING_BGP_ACT_SEND_OPEN);
    expect_state(ROUTING_BGP_OPENSENT);
    expect_deadline(200 + 240000);
    open_rx(LOWER_ID, HOLD, false, 300); /* not a collision: nothing was pending */
    expect_actions(1, ROUTING_BGP_ACT_SEND_KEEPALIVE);
    expect_state(ROUTING_BGP_OPENCONFIRM);

    /* accepted in Idle (CONNECTED incoming), replaced in OpenConfirm */
    fsm_init(HOLD);
    to_established(HOLD, 0);
    conn_lost(false, 1000);
    connected(true, 2000);
    open_rx(LOWER_ID, HOLD, false, 2000);
    simple(ROUTING_BGP_EV_INCOMING_WHILE_BUSY, 3000);
    expect_actions(2, ROUTING_BGP_ACT_ADOPT_INCOMING, ROUTING_BGP_ACT_SEND_OPEN);
    expect_state(ROUTING_BGP_OPENSENT);

    /* replaced in Established: counted as a reset */
    simple(ROUTING_BGP_EV_INCOMING_WHILE_BUSY, 4000);
    open_rx(LOWER_ID, HOLD, false, 4000);
    simple(ROUTING_BGP_EV_KEEPALIVE_RX, 4000);
    expect_state(ROUTING_BGP_ESTABLISHED);
    TEST_ASSERT_EQUAL_UINT32(1, routing_bgp_fsm_resets(&f));
    simple(ROUTING_BGP_EV_INCOMING_WHILE_BUSY, 5000);
    expect_actions(2, ROUTING_BGP_ACT_ADOPT_INCOMING, ROUTING_BGP_ACT_SEND_OPEN);
    expect_state(ROUTING_BGP_OPENSENT);
    TEST_ASSERT_EQUAL_UINT32(2, routing_bgp_fsm_resets(&f));
    TEST_ASSERT_EQUAL_STRING("connection lost", routing_bgp_fsm_last_error(&f));
    TEST_ASSERT_EQUAL_UINT32(0, routing_bgp_fsm_uptime_s(&f, 9000));

    /* an outgoing current connection still goes through collision resolution */
    fsm_init(HOLD);
    simple(ROUTING_BGP_EV_START, 0);
    connected(false, 0);
    simple(ROUTING_BGP_EV_INCOMING_WHILE_BUSY, 100);
    expect_actions(0);
    expect_state(ROUTING_BGP_OPENSENT);
}
