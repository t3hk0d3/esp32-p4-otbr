#include <string.h>
#include "unity.h"
#include "engine.h"
#include "settings.h"
#include "fake_io.h"

#define T0 1000u
#define LOCAL_ASN 65001u
#define P1_ASN 65002u
#define P2_ASN 65003u
#define BOARD_IPV4 0xc0a8140fu    /* 192.168.20.15 */
#define CONFIGURED_RID 0x0a0a0a0au /* 10.10.10.10 */
#define LOW_RID 0x0a000001u        /* below BOARD_IPV4 */
#define HIGH_RID 0xdf000001u       /* above BOARD_IPV4 */
#define CLIENT_PORT 12345

static const uint8_t P1[16] = {0x20, 0x01, 0x0d, 0xb8, 0x00, 0x42, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01};
static const uint8_t P2[16] = {0x20, 0x01, 0x0d, 0xb8, 0x00, 0x42, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x02};
static const uint8_t STRANGER[16] = {0x20, 0x01, 0x0d, 0xb8, 0x00, 0x42, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x99};
static const uint8_t CLIENT[16] = {0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0x02, 0x11, 0x22, 0xff, 0xfe, 0x33, 0x44, 0x55};
static const uint8_t LINK_LOCAL[16] = {0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x10};
static const uint8_t GLOBAL[16] = {0x20, 0x01, 0x0d, 0xb8, 0x00, 0x42, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x10};
static const routing_prefix64_t CUSTOM = {{0x20, 0x01, 0x0d, 0xb8, 0x00, 0x42, 0x01, 0x00}};
static const routing_prefix64_t ULA = {{0xfd, 0x98, 0x26, 0x56, 0x47, 0x40, 0x00, 0x01}};
static const routing_prefix64_t PD = {{0x20, 0x01, 0x0d, 0xb8, 0x00, 0x77, 0x00, 0x00}};
static const routing_prefix64_t OTHER = {{0x20, 0x01, 0x0d, 0xb8, 0x00, 0x99, 0x00, 0x00}};

static routing_engine_t s_e;

/* --- builders --- */

static routing_settings_t base_settings(routing_method_t method)
{
    routing_settings_t s;

    routing_settings_defaults(&s);
    s.method = method;
    s.announce_ula = true;
    s.local_asn = LOCAL_ASN;
    s.peer_count = 1;
    memcpy(s.peers[0].addr, P1, 16);
    s.peers[0].asn = P1_ASN;
    return s;
}

static routing_settings_t two_peers(void)
{
    routing_settings_t s = base_settings(ROUTING_METHOD_BGP);

    s.peer_count = 2;
    memcpy(s.peers[1].addr, P2, 16);
    s.peers[1].asn = P2_ASN;
    return s;
}

/* Border router up, ULA in Network Data, backbone IPv4 and link-local + global addresses. */
static routing_engine_state_t up_state(void)
{
    routing_engine_state_t st;

    memset(&st, 0, sizeof(st));
    st.br_up = true;
    st.ula_known = true;
    st.ula = ULA;
    st.ula_in_netdata = true;
    st.custom_in_netdata = true;
    st.backbone_ipv4 = BOARD_IPV4;
    st.have_link_local = true;
    memcpy(st.nexthop.link_local, LINK_LOCAL, 16);
    memcpy(st.nexthop.global, GLOBAL, 16);
    st.nexthop.has_global = true;
    return st;
}

static void begin(const routing_settings_t *s)
{
    fake_io_reset();
    routing_engine_init(&s_e, &fake_io, s);
    routing_engine_start(&s_e, T0);
}

static void poll_up(uint32_t now)
{
    routing_engine_state_t st = up_state();

    routing_engine_poll(&s_e, &st, now);
}

static void feed(int h, const uint8_t *msg, size_t len, uint32_t now)
{
    while (len > 0) {
        uint8_t *buf = NULL;
        size_t room = routing_engine_tcp_rx_space(&s_e, h, &buf);
        TEST_ASSERT_TRUE_MESSAGE(room > 0, "no room to receive");
        size_t n = len < room ? len : room;
        memcpy(buf, msg, n);
        routing_engine_tcp_rx(&s_e, h, (int)n, now);
        msg += n;
        len -= n;
    }
}

static void feed_open(int h, uint32_t asn, uint32_t rid, uint32_t now)
{
    uint8_t msg[64];
    size_t len = routing_bgp_encode_open(msg, sizeof(msg), asn, 90, rid);

    feed(h, msg, len, now);
}

static void feed_keepalive(int h, uint32_t now)
{
    uint8_t msg[ROUTING_BGP_HDR_LEN];

    feed(h, msg, routing_bgp_encode_keepalive(msg, sizeof(msg)), now);
}

/* The `nth` connect's handle, connected now. */
static int connected(int nth, uint32_t now)
{
    const fake_io_call_t *c = fake_io_find(FIO_CONNECT, nth);

    TEST_ASSERT_NOT_NULL(c);
    routing_engine_tcp_connected(&s_e, c->h, true, NULL, now);
    return c->h;
}

/* Brings the `nth` outgoing connection of the last poll to Established with a peer of `asn`. */
static int establish(int nth, uint32_t asn, uint32_t now)
{
    int h = connected(nth, now);

    feed_open(h, asn, LOW_RID, now);
    feed_keepalive(h, now);
    return h;
}

/* method bgp with peer P1, polled once, session Established; the call log is cleared. */
static int bgp_established(const routing_settings_t *s)
{
    begin(s);
    poll_up(T0);
    int h = establish(0, P1_ASN, T0);
    TEST_ASSERT_EQUAL(ROUTING_BGP_ESTABLISHED, routing_bgp_fsm_state(&s_e.peers[0].fsm));
    fake_io_clear_calls();
    return h;
}

/* The last NOTIFICATION sent on `h` (code << 8 | subcode), -1 when none. */
static int last_notification(int h)
{
    int found = -1;

    for (int i = 0; i < g_io.call_count; i++) {
        const fake_io_call_t *c = &g_io.calls[i];
        if (c->op == FIO_SEND && c->h == h && c->len >= 21 && c->data[18] == ROUTING_BGP_MSG_NOTIFICATION) {
            found = c->data[19] << 8 | c->data[20];
        }
    }
    return found;
}

static routing_prefix_set_t set_of(const routing_prefix64_t *a, const routing_prefix64_t *b)
{
    routing_prefix_set_t s = {0};

    if (a) {
        s.p[s.count++] = *a;
    }
    if (b) {
        s.p[s.count++] = *b;
    }
    return s;
}

static void assert_sent(const fake_io_call_t *c, const uint8_t *expected, size_t len)
{
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_EQUAL(len, c->len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, c->data, len);
}

/* The UPDATE this engine sends to P1 (eBGP) for `set`. */
static size_t expected_update(uint8_t *buf, size_t cap, const routing_prefix_set_t *set)
{
    routing_bgp_nexthop_t nh = up_state().nexthop;

    return routing_bgp_encode_update(buf, cap, false, LOCAL_ASN, &nh, set);
}

static size_t expected_ripng(uint8_t *buf, const routing_prefix64_t *p, uint8_t metric)
{
    routing_ripng_rte_t rte = {.prefix = *p, .metric = metric};
    size_t consumed;

    return routing_ripng_encode_response(buf, ROUTING_RIPNG_MAX_PAYLOAD, &rte, 1, &consumed);
}

static routing_status_t status_at(uint32_t now)
{
    routing_status_t st;

    routing_engine_status(&s_e, now, &st);
    return st;
}

/* --- BGP start --- */

TEST_CASE("Method bgp, first poll with a backbone IPv4: opens the listener and connects to every peer", "[routing]")
{
    routing_settings_t s = two_peers();

    begin(&s);
    poll_up(T0);
    TEST_ASSERT_EQUAL(1, fake_io_count(FIO_LISTEN));
    TEST_ASSERT_TRUE(fake_io_find(FIO_LISTEN, 0)->on);
    TEST_ASSERT_EQUAL(2, fake_io_count(FIO_CONNECT));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(P1, fake_io_find(FIO_CONNECT, 0)->addr, 16);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(P2, fake_io_find(FIO_CONNECT, 1)->addr, 16);
}

TEST_CASE("Method bgp, before the first poll: connects to nobody and does not listen", "[routing]")
{
    routing_settings_t s = base_settings(ROUTING_METHOD_BGP);

    begin(&s);
    routing_engine_tick(&s_e, T0 + 60000);
    TEST_ASSERT_EQUAL(0, fake_io_count(FIO_CONNECT));
    TEST_ASSERT_EQUAL(0, fake_io_count(FIO_LISTEN));
}

TEST_CASE("Method bgp, no IPv4 and no configured router ID: starts no session", "[routing]")
{
    routing_settings_t s = base_settings(ROUTING_METHOD_BGP);
    routing_engine_state_t st = up_state();

    st.backbone_ipv4 = 0;
    begin(&s);
    routing_engine_poll(&s_e, &st, T0);
    TEST_ASSERT_EQUAL(0, fake_io_count(FIO_CONNECT));
    TEST_ASSERT_EQUAL(0, fake_io_count(FIO_LISTEN));
}

TEST_CASE("Method bgp, no IPv4 and no configured router ID: status.bgp_blocked is \"no router id\"", "[routing]")
{
    routing_settings_t s = base_settings(ROUTING_METHOD_BGP);
    routing_engine_state_t st = up_state();

    st.backbone_ipv4 = 0;
    begin(&s);
    routing_engine_poll(&s_e, &st, T0);
    routing_status_t status = status_at(T0);
    TEST_ASSERT_EQUAL_STRING("no router id", status.bgp_blocked);
    TEST_ASSERT_EQUAL_UINT32(0, status.bgp_router_id);
}

TEST_CASE("Method bgp, router ID appears at a later poll: starts the sessions then", "[routing]")
{
    routing_settings_t s = base_settings(ROUTING_METHOD_BGP);
    routing_engine_state_t st = up_state();

    st.backbone_ipv4 = 0;
    begin(&s);
    routing_engine_poll(&s_e, &st, T0);
    poll_up(T0 + 500);
    TEST_ASSERT_EQUAL(1, fake_io_count(FIO_CONNECT));
    TEST_ASSERT_NULL(status_at(T0 + 500).bgp_blocked);
}

TEST_CASE("Method bgp, configured router ID and no IPv4: uses the configured router ID", "[routing]")
{
    routing_settings_t s = base_settings(ROUTING_METHOD_BGP);
    routing_engine_state_t st = up_state();

    s.router_id_set = true;
    s.router_id = CONFIGURED_RID;
    st.backbone_ipv4 = 0;
    begin(&s);
    routing_engine_poll(&s_e, &st, T0);
    int h = connected(0, T0);
    const fake_io_call_t *open = fake_io_send_on(h, 0);
    TEST_ASSERT_NOT_NULL(open);
    /* BGP identifier at offset 24 of the OPEN */
    TEST_ASSERT_EQUAL_HEX8_ARRAY(((uint8_t[]) {
        10, 10, 10, 10
    }), &open->data[24], 4);
    TEST_ASSERT_EQUAL_HEX32(CONFIGURED_RID, status_at(T0).bgp_router_id);
}

TEST_CASE("Method bgp, backbone IPv4 and a configured router ID: uses the backbone IPv4", "[routing]")
{
    routing_settings_t s = base_settings(ROUTING_METHOD_BGP);

    s.router_id_set = true;
    s.router_id = CONFIGURED_RID;
    begin(&s);
    poll_up(T0);
    TEST_ASSERT_EQUAL_HEX32(BOARD_IPV4, status_at(T0).bgp_router_id);
}

TEST_CASE("Method bgp, no link-local address: starts no session and reports it", "[routing]")
{
    routing_settings_t s = base_settings(ROUTING_METHOD_BGP);
    routing_engine_state_t st = up_state();

    st.have_link_local = false;
    begin(&s);
    routing_engine_poll(&s_e, &st, T0);
    TEST_ASSERT_EQUAL(0, fake_io_count(FIO_CONNECT));
    TEST_ASSERT_EQUAL_STRING("no link-local address", status_at(T0).bgp_blocked);
}

TEST_CASE("Backbone IPv4 changes while sessions run: restarts them with Cease 6/6", "[routing]")
{
    routing_settings_t s = base_settings(ROUTING_METHOD_BGP);
    int h = bgp_established(&s);
    routing_engine_state_t st = up_state();

    st.backbone_ipv4 = BOARD_IPV4 + 1;
    routing_engine_poll(&s_e, &st, T0 + 500);
    TEST_ASSERT_EQUAL_HEX(0x0606, last_notification(h));
    TEST_ASSERT_EQUAL(1, fake_io_closes(h));
    TEST_ASSERT_EQUAL(1, fake_io_count(FIO_CONNECT));
}

TEST_CASE("Listener fails to open: retried at the next poll", "[routing]")
{
    routing_settings_t s = base_settings(ROUTING_METHOD_BGP);

    begin(&s);
    g_io.listen_ok = false;
    poll_up(T0);
    g_io.listen_ok = true;
    poll_up(T0 + 500);
    TEST_ASSERT_EQUAL(2, fake_io_count(FIO_LISTEN));
    poll_up(T0 + 1000);
    TEST_ASSERT_EQUAL(2, fake_io_count(FIO_LISTEN));
}

/* --- BGP connect --- */

TEST_CASE("Connect fails at once: last_error is \"connect: <why>\" and the session is Active", "[routing]")
{
    routing_settings_t s = base_settings(ROUTING_METHOD_BGP);

    begin(&s);
    g_io.connect_fail = "host unreachable";
    poll_up(T0);
    routing_status_t st = status_at(T0);
    TEST_ASSERT_EQUAL(ROUTING_BGP_ACTIVE, st.peers[0].state);
    TEST_ASSERT_EQUAL_STRING("connect: host unreachable", st.peers[0].last_error);
}

TEST_CASE("Connect fails at once: retried after 5 s", "[routing]")
{
    routing_settings_t s = base_settings(ROUTING_METHOD_BGP);

    begin(&s);
    g_io.connect_fail = "host unreachable";
    poll_up(T0);
    g_io.connect_fail = NULL;
    routing_engine_tick(&s_e, T0 + 4999);
    TEST_ASSERT_EQUAL(1, fake_io_count(FIO_CONNECT));
    TEST_ASSERT_EQUAL_UINT32(T0 + 5000, routing_engine_next_deadline_ms(&s_e));
    routing_engine_tick(&s_e, T0 + 5000);
    TEST_ASSERT_EQUAL(2, fake_io_count(FIO_CONNECT));
}

TEST_CASE("Outgoing connection completes: sends OPEN on it", "[routing]")
{
    routing_settings_t s = base_settings(ROUTING_METHOD_BGP);
    uint8_t expected[64];

    begin(&s);
    poll_up(T0);
    int h = connected(0, T0);
    size_t len = routing_bgp_encode_open(expected, sizeof(expected), LOCAL_ASN, 90, BOARD_IPV4);
    assert_sent(fake_io_send_on(h, 0), expected, len);
    TEST_ASSERT_EQUAL(ROUTING_BGP_OPENSENT, status_at(T0).peers[0].state);
}

TEST_CASE("Outgoing attempt fails later: closes it and the session is Active", "[routing]")
{
    routing_settings_t s = base_settings(ROUTING_METHOD_BGP);

    begin(&s);
    poll_up(T0);
    int h = fake_io_find(FIO_CONNECT, 0)->h;
    routing_engine_tcp_connected(&s_e, h, false, "refused", T0 + 10);
    TEST_ASSERT_EQUAL(1, fake_io_closes(h));
    TEST_ASSERT_EQUAL(0, fake_io_sends_on(h));
    TEST_ASSERT_EQUAL_STRING("connect: refused", status_at(T0 + 10).peers[0].last_error);
}

TEST_CASE("No connection after 10 s: closes the attempt", "[routing]")
{
    routing_settings_t s = base_settings(ROUTING_METHOD_BGP);

    begin(&s);
    poll_up(T0);
    int h = fake_io_find(FIO_CONNECT, 0)->h;
    routing_engine_tick(&s_e, T0 + ROUTING_BGP_CONNECT_TIMEOUT_MS);
    TEST_ASSERT_EQUAL(1, fake_io_closes(h));
    TEST_ASSERT_EQUAL(ROUTING_BGP_ACTIVE, status_at(T0 + ROUTING_BGP_CONNECT_TIMEOUT_MS).peers[0].state);
}

/* --- BGP framing --- */

TEST_CASE("OPEN split across reads: nothing is sent until it is complete", "[routing]")
{
    routing_settings_t s = base_settings(ROUTING_METHOD_BGP);
    uint8_t msg[64];

    begin(&s);
    poll_up(T0);
    int h = connected(0, T0);
    size_t len = routing_bgp_encode_open(msg, sizeof(msg), P1_ASN, 90, LOW_RID);
    fake_io_clear_calls();
    feed(h, msg, 25, T0);
    TEST_ASSERT_EQUAL(0, fake_io_sends_on(h));
    feed(h, msg + 25, len - 25, T0);
    TEST_ASSERT_EQUAL(1, fake_io_bgp_sends(h, ROUTING_BGP_MSG_KEEPALIVE));
    TEST_ASSERT_EQUAL(ROUTING_BGP_OPENCONFIRM, status_at(T0).peers[0].state);
}

TEST_CASE("Partial header: no NOTIFICATION and the connection stays", "[routing]")
{
    routing_settings_t s = base_settings(ROUTING_METHOD_BGP);
    uint8_t msg[64];

    begin(&s);
    poll_up(T0);
    int h = connected(0, T0);
    routing_bgp_encode_open(msg, sizeof(msg), P1_ASN, 90, LOW_RID);
    fake_io_clear_calls();
    feed(h, msg, 5, T0);
    TEST_ASSERT_EQUAL(0, fake_io_sends_on(h));
    TEST_ASSERT_EQUAL(0, fake_io_closes(h));
    TEST_ASSERT_EQUAL(ROUTING_BGP_OPENSENT, status_at(T0).peers[0].state);
}

TEST_CASE("OPEN and KEEPALIVE in one read: both are handled", "[routing]")
{
    routing_settings_t s = base_settings(ROUTING_METHOD_BGP);
    uint8_t msg[96];

    begin(&s);
    poll_up(T0);
    int h = connected(0, T0);
    size_t len = routing_bgp_encode_open(msg, sizeof(msg), P1_ASN, 90, LOW_RID);
    len += routing_bgp_encode_keepalive(msg + len, sizeof(msg) - len);
    feed(h, msg, len, T0);
    TEST_ASSERT_EQUAL(ROUTING_BGP_ESTABLISHED, status_at(T0).peers[0].state);
}

TEST_CASE("Header with a bad marker: sends NOTIFICATION 1/1 and closes", "[routing]")
{
    routing_settings_t s = base_settings(ROUTING_METHOD_BGP);
    uint8_t msg[ROUTING_BGP_HDR_LEN];

    begin(&s);
    poll_up(T0);
    int h = connected(0, T0);
    routing_bgp_encode_keepalive(msg, sizeof(msg));
    msg[3] = 0;
    feed(h, msg, sizeof(msg), T0);
    TEST_ASSERT_EQUAL_HEX(0x0101, last_notification(h));
    TEST_ASSERT_EQUAL(1, fake_io_closes(h));
}

TEST_CASE("Header length above 4096: sends NOTIFICATION 1/2 and closes", "[routing]")
{
    routing_settings_t s = base_settings(ROUTING_METHOD_BGP);
    uint8_t msg[ROUTING_BGP_HDR_LEN];

    begin(&s);
    poll_up(T0);
    int h = connected(0, T0);
    routing_bgp_encode_keepalive(msg, sizeof(msg));
    msg[16] = 0x10;
    msg[17] = 0x01; /* 4097 */
    msg[18] = ROUTING_BGP_MSG_UPDATE;
    feed(h, msg, sizeof(msg), T0);
    TEST_ASSERT_EQUAL_HEX(0x0102, last_notification(h));
    TEST_ASSERT_EQUAL(1, fake_io_closes(h));
}

TEST_CASE("OPEN with an unexpected ASN: sends NOTIFICATION 2/2 and closes", "[routing]")
{
    routing_settings_t s = base_settings(ROUTING_METHOD_BGP);

    begin(&s);
    poll_up(T0);
    int h = connected(0, T0);
    feed_open(h, P2_ASN, LOW_RID, T0);
    TEST_ASSERT_EQUAL_HEX(0x0202, last_notification(h));
    TEST_ASSERT_EQUAL(1, fake_io_closes(h));
}

TEST_CASE("Malformed UPDATE in Established: sends NOTIFICATION 3/1 and closes", "[routing]")
{
    routing_settings_t s = base_settings(ROUTING_METHOD_BGP);
    int h = bgp_established(&s);
    uint8_t msg[23];

    memset(msg, 0xff, 16);
    msg[16] = 0;
    msg[17] = 23;
    msg[18] = ROUTING_BGP_MSG_UPDATE;
    msg[19] = 0x00;
    msg[20] = 0x10; /* withdrawn routes length 16, past the message */
    msg[21] = 0;
    msg[22] = 0;
    feed(h, msg, sizeof(msg), T0);
    TEST_ASSERT_EQUAL_HEX(0x0301, last_notification(h));
    TEST_ASSERT_EQUAL(1, fake_io_closes(h));
}

TEST_CASE("NOTIFICATION received: closes without sending and records it", "[routing]")
{
    routing_settings_t s = base_settings(ROUTING_METHOD_BGP);
    int h = bgp_established(&s);
    uint8_t msg[21];

    feed(h, msg, routing_bgp_encode_notification(msg, sizeof(msg), 6, 2), T0);
    TEST_ASSERT_EQUAL(0, fake_io_sends_on(h));
    TEST_ASSERT_EQUAL(1, fake_io_closes(h));
    TEST_ASSERT_EQUAL_STRING("notification 6/2 received", status_at(T0).peers[0].last_error);
}

/* --- BGP Established --- */

TEST_CASE("Established with an empty set: sends no UPDATE", "[routing]")
{
    routing_settings_t s = base_settings(ROUTING_METHOD_BGP);

    s.announce_ula = false;
    begin(&s);
    poll_up(T0);
    int h = establish(0, P1_ASN, T0);
    TEST_ASSERT_EQUAL(ROUTING_BGP_ESTABLISHED, status_at(T0).peers[0].state);
    TEST_ASSERT_EQUAL(0, fake_io_bgp_sends(h, ROUTING_BGP_MSG_UPDATE));
}

TEST_CASE("Established with a set: sends one UPDATE announcing it", "[routing]")
{
    routing_settings_t s = base_settings(ROUTING_METHOD_BGP);
    uint8_t expected[256];
    routing_prefix_set_t set = set_of(&ULA, NULL);

    begin(&s);
    poll_up(T0);
    int h = establish(0, P1_ASN, T0);
    TEST_ASSERT_EQUAL(1, fake_io_bgp_sends(h, ROUTING_BGP_MSG_UPDATE));
    size_t len = expected_update(expected, sizeof(expected), &set);
    assert_sent(fake_io_send_on(h, fake_io_sends_on(h) - 1), expected, len);
}

TEST_CASE("UPDATE received: received_dropped counts its prefixes and nothing is sent", "[routing]")
{
    routing_settings_t s = base_settings(ROUTING_METHOD_BGP);
    int h = bgp_established(&s);
    uint8_t msg[256];
    routing_bgp_nexthop_t nh = up_state().nexthop;
    routing_prefix_set_t set = set_of(&OTHER, &PD);

    feed(h, msg, routing_bgp_encode_update(msg, sizeof(msg), false, P1_ASN, &nh, &set), T0);
    TEST_ASSERT_EQUAL(0, fake_io_sends_on(h));
    TEST_ASSERT_EQUAL_UINT32(2, status_at(T0).peers[0].received_dropped);
}

TEST_CASE("Set grows: sends an UPDATE with only the added prefix", "[routing]")
{
    routing_settings_t s = base_settings(ROUTING_METHOD_BGP);
    uint8_t expected[256];
    routing_prefix_set_t added = set_of(&CUSTOM, NULL);

    s.omr_mode = ROUTING_OMR_CUSTOM;
    s.omr_prefix = CUSTOM;
    begin(&s);
    routing_engine_state_t st = up_state();
    st.custom_in_netdata = false;
    routing_engine_poll(&s_e, &st, T0);
    int h = establish(0, P1_ASN, T0);
    fake_io_clear_calls();
    st.custom_in_netdata = true;
    routing_engine_poll(&s_e, &st, T0 + 500);
    TEST_ASSERT_EQUAL(1, fake_io_sends_on(h));
    assert_sent(fake_io_send_on(h, 0), expected, expected_update(expected, sizeof(expected), &added));
}

TEST_CASE("Set shrinks: sends a withdrawal and no announcement", "[routing]")
{
    routing_settings_t s = base_settings(ROUTING_METHOD_BGP);
    uint8_t expected[256];
    routing_prefix_set_t removed = set_of(&ULA, NULL);

    int h = bgp_established(&s);
    routing_engine_state_t st = up_state();
    st.ula_in_netdata = false;
    routing_engine_poll(&s_e, &st, T0 + 500);
    TEST_ASSERT_EQUAL(1, fake_io_sends_on(h));
    assert_sent(fake_io_send_on(h, 0), expected, routing_bgp_encode_withdraw(expected, sizeof(expected), &removed));
}

/* Established with the ULA announced while the backbone had only its link-local address; calls cleared. */
static int established_link_local_only(void)
{
    routing_settings_t s = base_settings(ROUTING_METHOD_BGP);
    routing_engine_state_t st = up_state();

    st.nexthop.has_global = false;
    memset(st.nexthop.global, 0, 16);
    begin(&s);
    routing_engine_poll(&s_e, &st, T0);
    int h = establish(0, P1_ASN, T0);
    fake_io_clear_calls();
    return h;
}

TEST_CASE("Global address appears after Established: sends one UPDATE with the 32-byte next-hop", "[routing]")
{
    int h = established_link_local_only();
    uint8_t expected[256];
    routing_prefix_set_t set = set_of(&ULA, NULL);

    poll_up(T0 + 500);
    TEST_ASSERT_EQUAL(1, fake_io_sends_on(h));
    assert_sent(fake_io_send_on(h, 0), expected, expected_update(expected, sizeof(expected), &set));
    TEST_ASSERT_EQUAL(0, fake_io_closes(h));
    poll_up(T0 + 1000);
    TEST_ASSERT_EQUAL(1, fake_io_sends_on(h));
}

TEST_CASE("Global address disappears after Established: sends one UPDATE with the 16-byte next-hop", "[routing]")
{
    routing_settings_t s = base_settings(ROUTING_METHOD_BGP);
    int h = bgp_established(&s);
    uint8_t expected[256];
    routing_prefix_set_t set = set_of(&ULA, NULL);
    routing_engine_state_t st = up_state();

    st.nexthop.has_global = false;
    memset(st.nexthop.global, 0, 16);
    routing_engine_poll(&s_e, &st, T0 + 500);
    TEST_ASSERT_EQUAL(1, fake_io_sends_on(h));
    size_t len = routing_bgp_encode_update(expected, sizeof(expected), false, LOCAL_ASN, &st.nexthop, &set);
    assert_sent(fake_io_send_on(h, 0), expected, len);
    TEST_ASSERT_EQUAL(0, fake_io_closes(h));
}

TEST_CASE("Next-hop changes with nothing announced: sends nothing", "[routing]")
{
    routing_settings_t s = base_settings(ROUTING_METHOD_BGP);
    routing_engine_state_t st = up_state();

    s.announce_ula = false;
    st.nexthop.has_global = false;
    begin(&s);
    routing_engine_poll(&s_e, &st, T0);
    int h = establish(0, P1_ASN, T0);
    fake_io_clear_calls();
    poll_up(T0 + 500);
    TEST_ASSERT_EQUAL(0, fake_io_sends_on(h));
}

TEST_CASE("Keepalive timer: sends KEEPALIVE a third of the hold time later", "[routing]")
{
    routing_settings_t s = base_settings(ROUTING_METHOD_BGP);
    int h = bgp_established(&s);

    routing_engine_tick(&s_e, T0 + 29999);
    TEST_ASSERT_EQUAL(0, fake_io_sends_on(h));
    routing_engine_tick(&s_e, T0 + 30000);
    TEST_ASSERT_EQUAL(1, fake_io_bgp_sends(h, ROUTING_BGP_MSG_KEEPALIVE));
}

TEST_CASE("Connection lost in Established: closes it and retries after 5 s", "[routing]")
{
    routing_settings_t s = base_settings(ROUTING_METHOD_BGP);
    int h = bgp_established(&s);

    routing_engine_tcp_rx(&s_e, h, 0, T0 + 100);
    TEST_ASSERT_EQUAL(1, fake_io_closes(h));
    routing_status_t st = status_at(T0 + 100);
    TEST_ASSERT_EQUAL(ROUTING_BGP_IDLE, st.peers[0].state);
    TEST_ASSERT_EQUAL_UINT32(1, st.peers[0].resets);
    routing_engine_tick(&s_e, T0 + 5100);
    TEST_ASSERT_EQUAL(1, fake_io_count(FIO_CONNECT));
}

TEST_CASE("Send fails: handled as connection lost", "[routing]")
{
    routing_settings_t s = base_settings(ROUTING_METHOD_BGP);
    int h = bgp_established(&s);

    g_io.send_fail_h = h;
    routing_engine_tick(&s_e, T0 + 30000);
    TEST_ASSERT_EQUAL(1, fake_io_closes(h));
    TEST_ASSERT_EQUAL_STRING("connection lost", status_at(T0 + 30000).peers[0].last_error);
}

/* --- BGP accept --- */

TEST_CASE("Connection from a non-configured address: closed without sending anything", "[routing]")
{
    routing_settings_t s = base_settings(ROUTING_METHOD_BGP);

    begin(&s);
    poll_up(T0);
    fake_io_clear_calls();
    routing_engine_tcp_accepted(&s_e, 7, STRANGER, T0);
    TEST_ASSERT_EQUAL(0, fake_io_sends_on(7));
    TEST_ASSERT_EQUAL(1, fake_io_closes(7));
}

TEST_CASE("Connection while BGP does not run: closed without sending anything", "[routing]")
{
    routing_settings_t s = base_settings(ROUTING_METHOD_RIPNG);

    begin(&s);
    poll_up(T0);
    routing_engine_tcp_accepted(&s_e, 7, P1, T0);
    TEST_ASSERT_EQUAL(0, fake_io_sends_on(7));
    TEST_ASSERT_EQUAL(1, fake_io_closes(7));
}

TEST_CASE("Configured peer, no current connection: sends OPEN on the incoming connection", "[routing]")
{
    routing_settings_t s = base_settings(ROUTING_METHOD_BGP);

    begin(&s);
    g_io.connect_fail = "refused";
    poll_up(T0);
    routing_engine_tcp_accepted(&s_e, 7, P1, T0);
    TEST_ASSERT_EQUAL(1, fake_io_bgp_sends(7, ROUTING_BGP_MSG_OPEN));
    TEST_ASSERT_EQUAL(ROUTING_BGP_OPENSENT, status_at(T0).peers[0].state);
}

TEST_CASE("Configured peer while connecting out: closes the attempt and sends OPEN on the incoming", "[routing]")
{
    routing_settings_t s = base_settings(ROUTING_METHOD_BGP);

    begin(&s);
    poll_up(T0);
    int out = fake_io_find(FIO_CONNECT, 0)->h;
    routing_engine_tcp_accepted(&s_e, 7, P1, T0);
    TEST_ASSERT_EQUAL(1, fake_io_closes(out));
    TEST_ASSERT_EQUAL(1, fake_io_bgp_sends(7, ROUTING_BGP_MSG_OPEN));
}

TEST_CASE("Configured peer in OpenSent on an outgoing connection: holds it and sends nothing on it", "[routing]")
{
    routing_settings_t s = base_settings(ROUTING_METHOD_BGP);

    begin(&s);
    poll_up(T0);
    int out = connected(0, T0);
    routing_engine_tcp_accepted(&s_e, 7, P1, T0);
    TEST_ASSERT_EQUAL(0, fake_io_sends_on(7));
    TEST_ASSERT_EQUAL(0, fake_io_closes(7));
    TEST_ASSERT_EQUAL(0, fake_io_closes(out));
}

TEST_CASE("Second incoming connection while one is pending: closed without sending anything", "[routing]")
{
    routing_settings_t s = base_settings(ROUTING_METHOD_BGP);

    begin(&s);
    poll_up(T0);
    connected(0, T0);
    routing_engine_tcp_accepted(&s_e, 7, P1, T0);
    routing_engine_tcp_accepted(&s_e, 8, P1, T0);
    TEST_ASSERT_EQUAL(0, fake_io_sends_on(8));
    TEST_ASSERT_EQUAL(1, fake_io_closes(8));
    TEST_ASSERT_EQUAL(0, fake_io_closes(7));
}

TEST_CASE("Pending OPEN with a lower router ID: sends Cease 6/7 on the pending connection and closes it", "[routing]")
{
    routing_settings_t s = base_settings(ROUTING_METHOD_BGP);

    begin(&s);
    poll_up(T0);
    int out = connected(0, T0);
    routing_engine_tcp_accepted(&s_e, 7, P1, T0);
    feed_open(7, P1_ASN, LOW_RID, T0);
    TEST_ASSERT_EQUAL_HEX(0x0607, last_notification(7));
    TEST_ASSERT_EQUAL(1, fake_io_closes(7));
    TEST_ASSERT_EQUAL(0, fake_io_closes(out));
    TEST_ASSERT_EQUAL(ROUTING_BGP_OPENSENT, status_at(T0).peers[0].state);
}

TEST_CASE("Pending OPEN with a higher router ID: sends Cease 6/7 on the current connection and closes it",
          "[routing]")
{
    routing_settings_t s = base_settings(ROUTING_METHOD_BGP);

    begin(&s);
    poll_up(T0);
    int out = connected(0, T0);
    routing_engine_tcp_accepted(&s_e, 7, P1, T0);
    feed_open(7, P1_ASN, HIGH_RID, T0);
    TEST_ASSERT_EQUAL_HEX(0x0607, last_notification(out));
    TEST_ASSERT_EQUAL(1, fake_io_closes(out));
    TEST_ASSERT_EQUAL(0, fake_io_closes(7));
}

TEST_CASE("Pending OPEN with a higher router ID: sends OPEN and KEEPALIVE on the incoming connection", "[routing]")
{
    routing_settings_t s = base_settings(ROUTING_METHOD_BGP);

    begin(&s);
    poll_up(T0);
    connected(0, T0);
    routing_engine_tcp_accepted(&s_e, 7, P1, T0);
    feed_open(7, P1_ASN, HIGH_RID, T0);
    TEST_ASSERT_EQUAL(ROUTING_BGP_MSG_OPEN, fake_io_send_on(7, 0)->data[18]);
    TEST_ASSERT_EQUAL(ROUTING_BGP_MSG_KEEPALIVE, fake_io_send_on(7, 1)->data[18]);
}

TEST_CASE("Adopted incoming connection: its next KEEPALIVE establishes the session", "[routing]")
{
    routing_settings_t s = base_settings(ROUTING_METHOD_BGP);

    begin(&s);
    poll_up(T0);
    connected(0, T0);
    routing_engine_tcp_accepted(&s_e, 7, P1, T0);
    feed_open(7, P1_ASN, HIGH_RID, T0);
    feed_keepalive(7, T0);
    TEST_ASSERT_EQUAL(ROUTING_BGP_ESTABLISHED, status_at(T0).peers[0].state);
    TEST_ASSERT_EQUAL(1, fake_io_bgp_sends(7, ROUTING_BGP_MSG_UPDATE));
}

TEST_CASE("Pending connection closed by the peer: closed silently, the current session untouched", "[routing]")
{
    routing_settings_t s = base_settings(ROUTING_METHOD_BGP);

    begin(&s);
    poll_up(T0);
    int out = connected(0, T0);
    routing_engine_tcp_accepted(&s_e, 7, P1, T0);
    routing_engine_tcp_rx(&s_e, 7, 0, T0);
    TEST_ASSERT_EQUAL(0, fake_io_sends_on(7));
    TEST_ASSERT_EQUAL(1, fake_io_closes(7));
    TEST_ASSERT_EQUAL(0, fake_io_closes(out));
    TEST_ASSERT_EQUAL(ROUTING_BGP_OPENSENT, status_at(T0).peers[0].state);
}

TEST_CASE("Pending connection sends KEEPALIVE before its OPEN: sends NOTIFICATION 5/0 on it and closes it",
          "[routing]")
{
    routing_settings_t s = base_settings(ROUTING_METHOD_BGP);

    begin(&s);
    poll_up(T0);
    int out = connected(0, T0);
    routing_engine_tcp_accepted(&s_e, 7, P1, T0);
    feed_keepalive(7, T0);
    TEST_ASSERT_EQUAL_HEX(0x0500, last_notification(7));
    TEST_ASSERT_EQUAL(1, fake_io_closes(7));
    TEST_ASSERT_EQUAL(0, fake_io_closes(out));
}

TEST_CASE("Pending connection with a bad header: sends the NOTIFICATION on it, the current session untouched",
          "[routing]")
{
    routing_settings_t s = base_settings(ROUTING_METHOD_BGP);
    uint8_t msg[ROUTING_BGP_HDR_LEN];

    begin(&s);
    poll_up(T0);
    int out = connected(0, T0);
    routing_engine_tcp_accepted(&s_e, 7, P1, T0);
    routing_bgp_encode_keepalive(msg, sizeof(msg));
    msg[0] = 0;
    feed(7, msg, sizeof(msg), T0);
    TEST_ASSERT_EQUAL_HEX(0x0101, last_notification(7));
    TEST_ASSERT_EQUAL(1, fake_io_closes(7));
    TEST_ASSERT_EQUAL(0, fake_io_closes(out));
    TEST_ASSERT_EQUAL(-1, last_notification(out));
}

/* --- settings --- */

TEST_CASE("Method bgp to ripng: Cease 6/2 sent and the session closed before the RIPng socket opens", "[routing]")
{
    routing_settings_t s = base_settings(ROUTING_METHOD_BGP);
    int h = bgp_established(&s);
    routing_settings_t next = base_settings(ROUTING_METHOD_RIPNG);

    routing_engine_set_settings(&s_e, &next, T0 + 100);
    TEST_ASSERT_EQUAL_HEX(0x0602, last_notification(h));
    int opened = fake_io_index(FIO_RIPNG_SOCKET, 0);
    TEST_ASSERT_TRUE(opened >= 0);
    TEST_ASSERT_TRUE(fake_io_index(FIO_SEND, 0) < opened);
    TEST_ASSERT_TRUE(fake_io_index(FIO_CLOSE, 0) < opened);
}

TEST_CASE("Method bgp to ripng: the listener is closed before the RIPng socket opens", "[routing]")
{
    routing_settings_t s = base_settings(ROUTING_METHOD_BGP);
    routing_settings_t next = base_settings(ROUTING_METHOD_RIPNG);

    bgp_established(&s);
    routing_engine_set_settings(&s_e, &next, T0 + 100);
    const fake_io_call_t *listen = fake_io_find(FIO_LISTEN, 0);
    TEST_ASSERT_NOT_NULL(listen);
    TEST_ASSERT_FALSE(listen->on);
    TEST_ASSERT_TRUE(fake_io_index(FIO_LISTEN, 0) < fake_io_index(FIO_RIPNG_SOCKET, 0));
}

TEST_CASE("Method ripng to bgp: metric 16 multicast and the RIPng socket closed before the listener opens",
          "[routing]")
{
    routing_settings_t s = base_settings(ROUTING_METHOD_RIPNG);
    routing_settings_t next = base_settings(ROUTING_METHOD_BGP);
    uint8_t expected[ROUTING_RIPNG_MAX_PAYLOAD];

    begin(&s);
    poll_up(T0);
    fake_io_clear_calls();
    routing_engine_set_settings(&s_e, &next, T0 + 100);
    const fake_io_call_t *withdraw = fake_io_find(FIO_RIPNG_SEND, 0);
    assert_sent(withdraw, expected, expected_ripng(expected, &ULA, ROUTING_RIPNG_INFINITY));
    TEST_ASSERT_FALSE(withdraw->has_addr);
    int closed = fake_io_index(FIO_RIPNG_SOCKET, 0);
    TEST_ASSERT_FALSE(g_io.calls[closed].on);
    TEST_ASSERT_TRUE(fake_io_index(FIO_RIPNG_SEND, 0) < closed);
    TEST_ASSERT_TRUE(closed < fake_io_index(FIO_LISTEN, 0));
}

TEST_CASE("Method change: the publisher applies the new settings after the old sender stops and before the new "
          "one starts", "[routing]")
{
    routing_settings_t s = base_settings(ROUTING_METHOD_RIPNG);
    routing_settings_t next = base_settings(ROUTING_METHOD_BGP);

    begin(&s);
    poll_up(T0);
    fake_io_clear_calls();
    routing_engine_set_settings(&s_e, &next, T0 + 100);
    TEST_ASSERT_EQUAL(1, fake_io_count(FIO_PUBLISHER_APPLY));
    int applied = fake_io_index(FIO_PUBLISHER_APPLY, 0);
    TEST_ASSERT_EQUAL(ROUTING_METHOD_BGP, g_io.calls[applied].settings.method);
    TEST_ASSERT_TRUE(fake_io_index(FIO_RIPNG_SOCKET, 0) < applied);
    TEST_ASSERT_TRUE(applied < fake_io_index(FIO_LISTEN, 0));
}

TEST_CASE("Method pd to ripng: opens the RIPng socket after the publisher turned PD off", "[routing]")
{
    routing_settings_t s = base_settings(ROUTING_METHOD_PD);
    routing_settings_t next = base_settings(ROUTING_METHOD_RIPNG);

    begin(&s);
    poll_up(T0);
    TEST_ASSERT_EQUAL(0, fake_io_count(FIO_RIPNG_SOCKET));
    routing_engine_set_settings(&s_e, &next, T0 + 100);
    TEST_ASSERT_TRUE(fake_io_index(FIO_PUBLISHER_APPLY, 0) < fake_io_index(FIO_RIPNG_SOCKET, 0));
    TEST_ASSERT_TRUE(fake_io_find(FIO_RIPNG_SOCKET, 0)->on);
}

TEST_CASE("Only announce_ula changed with BGP established: sends an UPDATE at once without a reset", "[routing]")
{
    routing_settings_t s = base_settings(ROUTING_METHOD_BGP);
    routing_settings_t next = s;

    s.announce_ula = false;
    int h = bgp_established(&s);
    routing_engine_set_settings(&s_e, &next, T0 + 100);
    TEST_ASSERT_EQUAL(1, fake_io_bgp_sends(h, ROUTING_BGP_MSG_UPDATE));
    TEST_ASSERT_EQUAL(-1, last_notification(h));
    TEST_ASSERT_EQUAL(0, fake_io_closes(h));
}

TEST_CASE("Only omr changed with BGP established: sends an UPDATE once the prefix is in Network Data, no reset",
          "[routing]")
{
    routing_settings_t s = base_settings(ROUTING_METHOD_BGP);
    routing_settings_t next = s;
    uint8_t expected[256];
    routing_prefix_set_t added = set_of(&CUSTOM, NULL);

    next.omr_mode = ROUTING_OMR_CUSTOM;
    next.omr_prefix = CUSTOM;
    int h = bgp_established(&s);
    routing_engine_set_settings(&s_e, &next, T0 + 100);
    TEST_ASSERT_EQUAL(0, fake_io_sends_on(h));
    poll_up(T0 + 500);
    TEST_ASSERT_EQUAL(1, fake_io_sends_on(h));
    assert_sent(fake_io_send_on(h, 0), expected, expected_update(expected, sizeof(expected), &added));
    TEST_ASSERT_EQUAL(0, fake_io_closes(h));
    TEST_ASSERT_EQUAL(0, fake_io_count(FIO_CONNECT));
}

TEST_CASE("Only prefixes changed: applies the publisher once with the new settings", "[routing]")
{
    routing_settings_t s = base_settings(ROUTING_METHOD_BGP);
    routing_settings_t next = s;

    next.announce_ula = false;
    bgp_established(&s);
    routing_engine_set_settings(&s_e, &next, T0 + 100);
    TEST_ASSERT_EQUAL(1, fake_io_count(FIO_PUBLISHER_APPLY));
    TEST_ASSERT_FALSE(fake_io_find(FIO_PUBLISHER_APPLY, 0)->settings.announce_ula);
}

TEST_CASE("Only announce_ula changed with RIPng: sends a triggered update without reopening the socket",
          "[routing]")
{
    routing_settings_t s = base_settings(ROUTING_METHOD_RIPNG);
    routing_settings_t next = s;
    uint8_t expected[ROUTING_RIPNG_MAX_PAYLOAD];

    next.announce_ula = false;
    begin(&s);
    poll_up(T0);
    fake_io_clear_calls();
    routing_engine_set_settings(&s_e, &next, T0 + 100);
    routing_engine_tick(&s_e, T0 + 100);
    TEST_ASSERT_EQUAL(1, fake_io_count(FIO_RIPNG_SEND));
    assert_sent(fake_io_find(FIO_RIPNG_SEND, 0), expected, expected_ripng(expected, &ULA, ROUTING_RIPNG_INFINITY));
    TEST_ASSERT_EQUAL(0, fake_io_count(FIO_RIPNG_SOCKET));
}

TEST_CASE("Peer removed: sends Cease 6/3 to it and closes it, the other session untouched", "[routing]")
{
    routing_settings_t s = two_peers();
    routing_settings_t next = s;

    begin(&s);
    poll_up(T0);
    int h1 = establish(0, P1_ASN, T0);
    int h2 = establish(1, P2_ASN, T0);
    fake_io_clear_calls();
    next.peer_count = 1;
    next.peers[0] = s.peers[1];
    routing_engine_set_settings(&s_e, &next, T0 + 100);
    TEST_ASSERT_EQUAL_HEX(0x0603, last_notification(h1));
    TEST_ASSERT_EQUAL(1, fake_io_closes(h1));
    TEST_ASSERT_EQUAL(0, fake_io_sends_on(h2));
    TEST_ASSERT_EQUAL(0, fake_io_closes(h2));
    TEST_ASSERT_EQUAL(ROUTING_BGP_ESTABLISHED, status_at(T0 + 100).peers[0].state);
}

TEST_CASE("Peer ASN changed: sends Cease 6/3 and connects to it again", "[routing]")
{
    routing_settings_t s = base_settings(ROUTING_METHOD_BGP);
    routing_settings_t next = s;

    next.peers[0].asn = P2_ASN;
    int h = bgp_established(&s);
    routing_engine_set_settings(&s_e, &next, T0 + 100);
    TEST_ASSERT_EQUAL_HEX(0x0603, last_notification(h));
    TEST_ASSERT_EQUAL(1, fake_io_closes(h));
    TEST_ASSERT_EQUAL(1, fake_io_count(FIO_CONNECT));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(P1, fake_io_find(FIO_CONNECT, 0)->addr, 16);
}

TEST_CASE("Peer added: connects to the new peer only", "[routing]")
{
    routing_settings_t s = base_settings(ROUTING_METHOD_BGP);
    routing_settings_t next = two_peers();
    int h = bgp_established(&s);

    routing_engine_set_settings(&s_e, &next, T0 + 100);
    TEST_ASSERT_EQUAL(1, fake_io_count(FIO_CONNECT));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(P2, fake_io_find(FIO_CONNECT, 0)->addr, 16);
    TEST_ASSERT_EQUAL(0, fake_io_sends_on(h));
    TEST_ASSERT_EQUAL(0, fake_io_count(FIO_PUBLISHER_APPLY));
}

TEST_CASE("Local ASN changed: restarts every session with Cease 6/6", "[routing]")
{
    routing_settings_t s = two_peers();
    routing_settings_t next = s;

    begin(&s);
    poll_up(T0);
    int h1 = establish(0, P1_ASN, T0);
    int h2 = establish(1, P2_ASN, T0);
    fake_io_clear_calls();
    next.local_asn = LOCAL_ASN + 100;
    routing_engine_set_settings(&s_e, &next, T0 + 100);
    TEST_ASSERT_EQUAL_HEX(0x0606, last_notification(h1));
    TEST_ASSERT_EQUAL_HEX(0x0606, last_notification(h2));
    TEST_ASSERT_EQUAL(2, fake_io_count(FIO_CONNECT));
}

TEST_CASE("Peers reordered: sessions kept, status follows the new order", "[routing]")
{
    routing_settings_t s = two_peers();
    routing_settings_t next = s;

    begin(&s);
    poll_up(T0);
    int h1 = establish(0, P1_ASN, T0);
    connected(1, T0);
    fake_io_clear_calls();
    next.peers[0] = s.peers[1];
    next.peers[1] = s.peers[0];
    routing_engine_set_settings(&s_e, &next, T0 + 100);
    TEST_ASSERT_EQUAL(0, g_io.call_count);
    routing_status_t st = status_at(T0 + 100);
    TEST_ASSERT_EQUAL(ROUTING_BGP_OPENSENT, st.peers[0].state);
    TEST_ASSERT_EQUAL(ROUTING_BGP_ESTABLISHED, st.peers[1].state);
    (void)h1;
}

TEST_CASE("Same settings again: nothing happens", "[routing]")
{
    routing_settings_t s = base_settings(ROUTING_METHOD_BGP);

    bgp_established(&s);
    routing_engine_set_settings(&s_e, &s, T0 + 100);
    TEST_ASSERT_EQUAL(0, g_io.call_count);
}

TEST_CASE("Publisher apply fails: retried at the next poll", "[routing]")
{
    routing_settings_t s = base_settings(ROUTING_METHOD_PD);
    routing_settings_t next = s;

    next.announce_ula = false;
    begin(&s);
    poll_up(T0);
    g_io.publisher_ok = false;
    routing_engine_set_settings(&s_e, &next, T0 + 100);
    g_io.publisher_ok = true;
    poll_up(T0 + 500);
    TEST_ASSERT_EQUAL(2, fake_io_count(FIO_PUBLISHER_APPLY));
    poll_up(T0 + 1000);
    TEST_ASSERT_EQUAL(2, fake_io_count(FIO_PUBLISHER_APPLY));
}

TEST_CASE("Session events: never apply the publisher", "[routing]")
{
    routing_settings_t s = base_settings(ROUTING_METHOD_BGP);
    uint8_t msg[256];
    routing_bgp_nexthop_t nh = up_state().nexthop;
    routing_prefix_set_t set = set_of(&OTHER, NULL);

    begin(&s);
    poll_up(T0);
    int h = establish(0, P1_ASN, T0);
    feed(h, msg, routing_bgp_encode_update(msg, sizeof(msg), false, P1_ASN, &nh, &set), T0);
    routing_engine_tcp_rx(&s_e, h, -1, T0 + 10);
    routing_engine_tick(&s_e, T0 + 6000);
    routing_engine_tcp_accepted(&s_e, 7, STRANGER, T0 + 6000);
    poll_up(T0 + 6500);
    TEST_ASSERT_EQUAL(0, fake_io_count(FIO_PUBLISHER_APPLY));
}

TEST_CASE("Start: does not apply the publisher", "[routing]")
{
    routing_settings_t s = base_settings(ROUTING_METHOD_RIPNG);

    begin(&s);
    poll_up(T0);
    TEST_ASSERT_EQUAL(0, fake_io_count(FIO_PUBLISHER_APPLY));
}

/* --- RIPng --- */

TEST_CASE("Method ripng, border router up at the first poll: multicasts the set at metric 1", "[routing]")
{
    routing_settings_t s = base_settings(ROUTING_METHOD_RIPNG);
    uint8_t expected[ROUTING_RIPNG_MAX_PAYLOAD];

    begin(&s);
    TEST_ASSERT_TRUE(fake_io_find(FIO_RIPNG_SOCKET, 0)->on);
    TEST_ASSERT_EQUAL(0, fake_io_count(FIO_RIPNG_SEND));
    poll_up(T0);
    const fake_io_call_t *c = fake_io_find(FIO_RIPNG_SEND, 0);
    assert_sent(c, expected, expected_ripng(expected, &ULA, ROUTING_RIPNG_METRIC));
    TEST_ASSERT_FALSE(c->has_addr);
}

TEST_CASE("Regular update: multicast 30 s later, counted in updates_sent", "[routing]")
{
    routing_settings_t s = base_settings(ROUTING_METHOD_RIPNG);

    begin(&s);
    poll_up(T0);
    routing_engine_tick(&s_e, T0 + 29999);
    TEST_ASSERT_EQUAL(1, fake_io_count(FIO_RIPNG_SEND));
    routing_engine_tick(&s_e, T0 + 30000);
    TEST_ASSERT_EQUAL(2, fake_io_count(FIO_RIPNG_SEND));
    routing_status_t st = status_at(T0 + 30250);
    TEST_ASSERT_EQUAL_UINT32(2, st.ripng.updates_sent);
    TEST_ASSERT_EQUAL_INT64(250, st.ripng.last_update_ms_ago);
}

TEST_CASE("RIPng never sent: last_update_ms_ago is -1", "[routing]")
{
    routing_settings_t s = base_settings(ROUTING_METHOD_RIPNG);

    begin(&s);
    TEST_ASSERT_EQUAL_INT64(-1, status_at(T0).ripng.last_update_ms_ago);
}

TEST_CASE("Whole-table request from port 12345: answered unicast to that address and port with the table",
          "[routing]")
{
    routing_settings_t s = base_settings(ROUTING_METHOD_RIPNG);
    uint8_t req[24] = {1, 1, 0, 0};
    uint8_t expected[ROUTING_RIPNG_MAX_PAYLOAD];

    req[23] = ROUTING_RIPNG_INFINITY;
    begin(&s);
    poll_up(T0);
    fake_io_clear_calls();
    routing_engine_ripng_rx(&s_e, CLIENT, CLIENT_PORT, req, sizeof(req), T0 + 10);
    const fake_io_call_t *c = fake_io_find(FIO_RIPNG_SEND, 0);
    assert_sent(c, expected, expected_ripng(expected, &ULA, ROUTING_RIPNG_METRIC));
    TEST_ASSERT_TRUE(c->has_addr);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(CLIENT, c->addr, 16);
    TEST_ASSERT_EQUAL_UINT16(CLIENT_PORT, c->port);
}

TEST_CASE("Whole-table request: counted in requests_answered", "[routing]")
{
    routing_settings_t s = base_settings(ROUTING_METHOD_RIPNG);
    uint8_t req[24] = {1, 1, 0, 0};

    req[23] = ROUTING_RIPNG_INFINITY;
    begin(&s);
    poll_up(T0);
    routing_engine_ripng_rx(&s_e, CLIENT, ROUTING_RIPNG_PORT, req, sizeof(req), T0 + 10);
    TEST_ASSERT_EQUAL_UINT32(1, status_at(T0 + 10).ripng.requests_answered);
}

TEST_CASE("Specific request: answered to the requester with metric 1 for an announced prefix, 16 otherwise",
          "[routing]")
{
    routing_settings_t s = base_settings(ROUTING_METHOD_RIPNG);
    uint8_t req[44] = {1, 1, 0, 0};

    memcpy(&req[4], ULA.b, 8);
    req[22] = 64;
    req[23] = ROUTING_RIPNG_INFINITY;
    memcpy(&req[24], OTHER.b, 8);
    req[42] = 64;
    req[43] = ROUTING_RIPNG_INFINITY;
    begin(&s);
    poll_up(T0);
    fake_io_clear_calls();
    routing_engine_ripng_rx(&s_e, CLIENT, CLIENT_PORT, req, sizeof(req), T0 + 10);
    const fake_io_call_t *c = fake_io_find(FIO_RIPNG_SEND, 0);
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_EQUAL(sizeof(req), c->len);
    TEST_ASSERT_EQUAL_UINT8(2, c->data[0]);
    TEST_ASSERT_EQUAL_UINT8(ROUTING_RIPNG_METRIC, c->data[23]);
    TEST_ASSERT_EQUAL_UINT8(ROUTING_RIPNG_INFINITY, c->data[43]);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(CLIENT, c->addr, 16);
    TEST_ASSERT_EQUAL_UINT16(CLIENT_PORT, c->port);
    TEST_ASSERT_EQUAL_UINT32(1, status_at(T0 + 10).ripng.requests_answered);
}

TEST_CASE("Specific request: the request is left unchanged (answer built in its own buffer)", "[routing]")
{
    routing_settings_t s = base_settings(ROUTING_METHOD_RIPNG);
    uint8_t req[24] = {1, 1, 0, 0};
    uint8_t copy[24];

    memcpy(&req[4], ULA.b, 8);
    req[22] = 64;
    req[23] = ROUTING_RIPNG_INFINITY;
    memcpy(copy, req, sizeof(req));
    begin(&s);
    poll_up(T0);
    routing_engine_ripng_rx(&s_e, CLIENT, CLIENT_PORT, req, sizeof(req), T0 + 10);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(copy, req, sizeof(req));
}

TEST_CASE("Response datagram: counted in responses_dropped, nothing sent", "[routing]")
{
    routing_settings_t s = base_settings(ROUTING_METHOD_RIPNG);
    uint8_t resp[ROUTING_RIPNG_MAX_PAYLOAD];
    size_t len = expected_ripng(resp, &OTHER, 1);

    begin(&s);
    poll_up(T0);
    fake_io_clear_calls();
    routing_engine_ripng_rx(&s_e, CLIENT, ROUTING_RIPNG_PORT, resp, len, T0 + 10);
    TEST_ASSERT_EQUAL(0, g_io.call_count);
    TEST_ASSERT_EQUAL_UINT32(1, status_at(T0 + 10).ripng.responses_dropped);
}

TEST_CASE("Invalid datagram: counted in responses_dropped, nothing sent", "[routing]")
{
    routing_settings_t s = base_settings(ROUTING_METHOD_RIPNG);
    uint8_t bad[10] = {1, 1, 0, 0, 1, 2, 3, 4, 5, 6}; /* a partial RTE */

    begin(&s);
    poll_up(T0);
    fake_io_clear_calls();
    routing_engine_ripng_rx(&s_e, CLIENT, ROUTING_RIPNG_PORT, bad, sizeof(bad), T0 + 10);
    TEST_ASSERT_EQUAL(0, g_io.call_count);
    TEST_ASSERT_EQUAL_UINT32(1, status_at(T0 + 10).ripng.responses_dropped);
}

TEST_CASE("Neighbour floods 1000 responses: every one counted, nothing sent", "[routing]")
{
    routing_settings_t s = base_settings(ROUTING_METHOD_RIPNG);
    uint8_t resp[ROUTING_RIPNG_MAX_PAYLOAD];
    size_t len = expected_ripng(resp, &OTHER, 1);

    begin(&s);
    poll_up(T0);
    fake_io_clear_calls();
    for (int i = 0; i < 1000; i++) {
        routing_engine_ripng_rx(&s_e, CLIENT, ROUTING_RIPNG_PORT, resp, len, T0 + 10);
    }
    TEST_ASSERT_EQUAL(0, g_io.call_count);
    TEST_ASSERT_EQUAL_UINT32(1000, status_at(T0 + 10).ripng.responses_dropped);
}

TEST_CASE("Method not ripng: RIPng datagrams are ignored", "[routing]")
{
    routing_settings_t s = base_settings(ROUTING_METHOD_PD);
    uint8_t req[24] = {1, 1, 0, 0};

    req[23] = ROUTING_RIPNG_INFINITY;
    begin(&s);
    poll_up(T0);
    routing_engine_ripng_rx(&s_e, CLIENT, ROUTING_RIPNG_PORT, req, sizeof(req), T0 + 10);
    TEST_ASSERT_EQUAL(0, fake_io_count(FIO_RIPNG_SEND));
    TEST_ASSERT_EQUAL_UINT32(0, status_at(T0 + 10).ripng.requests_answered);
}

TEST_CASE("RIPng socket fails to open: retried at the next poll, then the set is multicast", "[routing]")
{
    routing_settings_t s = base_settings(ROUTING_METHOD_RIPNG);

    fake_io_reset();
    g_io.ripng_ok = false;
    routing_engine_init(&s_e, &fake_io, &s);
    routing_engine_start(&s_e, T0);
    poll_up(T0);
    TEST_ASSERT_EQUAL(0, fake_io_count(FIO_RIPNG_SEND));
    g_io.ripng_ok = true;
    poll_up(T0 + 500);
    TEST_ASSERT_EQUAL(3, fake_io_count(FIO_RIPNG_SOCKET));
    TEST_ASSERT_EQUAL(1, fake_io_count(FIO_RIPNG_SEND));
}

/* --- border router down --- */

TEST_CASE("Border router stops with BGP established: withdraws every prefix in the same poll", "[routing]")
{
    routing_settings_t s = base_settings(ROUTING_METHOD_BGP);
    uint8_t expected[256];
    routing_prefix_set_t removed = set_of(&ULA, NULL);
    int h = bgp_established(&s);
    routing_engine_state_t st = up_state();

    st.br_up = false;
    routing_engine_poll(&s_e, &st, T0 + 500);
    assert_sent(fake_io_send_on(h, 0), expected, routing_bgp_encode_withdraw(expected, sizeof(expected), &removed));
    TEST_ASSERT_EQUAL(0, fake_io_closes(h));
}

TEST_CASE("Border router stops with RIPng: multicasts metric 16 in the same poll", "[routing]")
{
    routing_settings_t s = base_settings(ROUTING_METHOD_RIPNG);
    uint8_t expected[ROUTING_RIPNG_MAX_PAYLOAD];
    routing_engine_state_t st = up_state();

    begin(&s);
    routing_engine_poll(&s_e, &st, T0);
    /* a triggered update just went out: the 5 s gap must not delay the withdrawal */
    routing_settings_t next = s;
    next.announce_ula = false;
    routing_engine_set_settings(&s_e, &next, T0 + 100);
    routing_engine_set_settings(&s_e, &s, T0 + 200);
    routing_engine_tick(&s_e, T0 + 200);
    fake_io_clear_calls();
    st.br_up = false;
    routing_engine_poll(&s_e, &st, T0 + 500);
    assert_sent(fake_io_find(FIO_RIPNG_SEND, 0), expected, expected_ripng(expected, &ULA, ROUTING_RIPNG_INFINITY));
    TEST_ASSERT_EQUAL(0, fake_io_count(FIO_RIPNG_SOCKET));
}

TEST_CASE("Border router back up with RIPng: multicasts the set at metric 1 again", "[routing]")
{
    routing_settings_t s = base_settings(ROUTING_METHOD_RIPNG);
    uint8_t expected[ROUTING_RIPNG_MAX_PAYLOAD];
    routing_engine_state_t st = up_state();

    begin(&s);
    routing_engine_poll(&s_e, &st, T0);
    st.br_up = false;
    routing_engine_poll(&s_e, &st, T0 + 500);
    fake_io_clear_calls();
    st.br_up = true;
    routing_engine_poll(&s_e, &st, T0 + 1000);
    assert_sent(fake_io_find(FIO_RIPNG_SEND, 0), expected, expected_ripng(expected, &ULA, ROUTING_RIPNG_METRIC));
}

TEST_CASE("Border router down with RIPng: no regular updates while it stays down", "[routing]")
{
    routing_settings_t s = base_settings(ROUTING_METHOD_RIPNG);
    routing_engine_state_t st = up_state();

    begin(&s);
    routing_engine_poll(&s_e, &st, T0);
    st.br_up = false;
    routing_engine_poll(&s_e, &st, T0 + 500);
    fake_io_clear_calls();
    routing_engine_tick(&s_e, T0 + 120000);
    TEST_ASSERT_EQUAL(0, fake_io_count(FIO_RIPNG_SEND));
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, routing_engine_next_deadline_ms(&s_e));
}

/* --- status --- */

TEST_CASE("OMR custom: omr_prefix is the custom prefix", "[routing]")
{
    routing_settings_t s = base_settings(ROUTING_METHOD_PD);
    routing_engine_state_t st = up_state();

    s.omr_mode = ROUTING_OMR_CUSTOM;
    s.omr_prefix = CUSTOM;
    st.pd_known = true;
    st.pd_prefix = PD;
    begin(&s);
    routing_engine_poll(&s_e, &st, T0);
    routing_status_t status = status_at(T0);
    TEST_ASSERT_TRUE(status.omr_prefix_known);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(CUSTOM.b, status.omr_prefix.b, 8);
}

TEST_CASE("OMR auto with a delegated prefix: omr_prefix is the delegated prefix", "[routing]")
{
    routing_settings_t s = base_settings(ROUTING_METHOD_PD);
    routing_engine_state_t st = up_state();

    st.pd_known = true;
    st.pd_prefix = PD;
    begin(&s);
    routing_engine_poll(&s_e, &st, T0);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(PD.b, status_at(T0).omr_prefix.b, 8);
}

TEST_CASE("OMR auto without delegation: omr_prefix and ula_prefix are the ULA", "[routing]")
{
    routing_settings_t s = base_settings(ROUTING_METHOD_PD);

    begin(&s);
    poll_up(T0);
    routing_status_t status = status_at(T0);
    TEST_ASSERT_TRUE(status.omr_prefix_known);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(ULA.b, status.omr_prefix.b, 8);
    TEST_ASSERT_TRUE(status.ula_prefix_known);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(ULA.b, status.ula_prefix.b, 8);
}

TEST_CASE("Before the first poll: omr_prefix and ula_prefix are unknown", "[routing]")
{
    routing_settings_t s = base_settings(ROUTING_METHOD_PD);

    begin(&s);
    routing_status_t status = status_at(T0);
    TEST_ASSERT_FALSE(status.omr_prefix_known);
    TEST_ASSERT_FALSE(status.ula_prefix_known);
}

TEST_CASE("Status: announced is the current announcement set", "[routing]")
{
    routing_settings_t s = base_settings(ROUTING_METHOD_RIPNG);

    begin(&s);
    poll_up(T0);
    routing_status_t status = status_at(T0);
    TEST_ASSERT_EQUAL_UINT8(1, status.announced.count);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(ULA.b, status.announced.p[0].b, 8);
}

TEST_CASE("Method pd: announces nothing", "[routing]")
{
    routing_settings_t s = base_settings(ROUTING_METHOD_PD);

    begin(&s);
    poll_up(T0);
    TEST_ASSERT_EQUAL_UINT8(0, status_at(T0).announced.count);
}

TEST_CASE("Method not bgp: bgp_blocked is NULL even without a router ID", "[routing]")
{
    routing_settings_t s = base_settings(ROUTING_METHOD_RIPNG);
    routing_engine_state_t st = up_state();

    st.backbone_ipv4 = 0;
    begin(&s);
    routing_engine_poll(&s_e, &st, T0);
    TEST_ASSERT_NULL(status_at(T0).bgp_blocked);
}

TEST_CASE("Established for 125 s: status uptime_s is 125", "[routing]")
{
    routing_settings_t s = base_settings(ROUTING_METHOD_BGP);

    bgp_established(&s);
    TEST_ASSERT_EQUAL_UINT32(125, status_at(T0 + 125000).peers[0].uptime_s);
}

TEST_CASE("Metrics view: method, announced count, omr_custom, peers and RIPng counters", "[routing]")
{
    routing_settings_t s = base_settings(ROUTING_METHOD_BGP);
    routing_metrics_view_t v;

    s.omr_mode = ROUTING_OMR_CUSTOM;
    s.omr_prefix = CUSTOM;
    bgp_established(&s);
    routing_engine_metrics_view(&s_e, T0 + 2000, &v);
    TEST_ASSERT_EQUAL(ROUTING_METHOD_BGP, v.method);
    TEST_ASSERT_EQUAL_UINT8(2, v.announced_count);
    TEST_ASSERT_TRUE(v.omr_custom);
    TEST_ASSERT_EQUAL_UINT8(1, v.peer_count);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(P1, v.peers[0].addr, 16);
    TEST_ASSERT_EQUAL(ROUTING_BGP_ESTABLISHED, v.peers[0].state);
    TEST_ASSERT_EQUAL_UINT32(2, v.peers[0].uptime_s);
    TEST_ASSERT_EQUAL_UINT32(0, v.ripng_updates_sent);
}

TEST_CASE("Metrics view with RIPng: counts updates, requests and dropped responses", "[routing]")
{
    routing_settings_t s = base_settings(ROUTING_METHOD_RIPNG);
    uint8_t req[24] = {1, 1, 0, 0};
    uint8_t resp[ROUTING_RIPNG_MAX_PAYLOAD];
    size_t len = expected_ripng(resp, &OTHER, 1);
    routing_metrics_view_t v;

    req[23] = ROUTING_RIPNG_INFINITY;
    begin(&s);
    poll_up(T0);
    routing_engine_ripng_rx(&s_e, CLIENT, ROUTING_RIPNG_PORT, req, sizeof(req), T0 + 10);
    routing_engine_ripng_rx(&s_e, CLIENT, ROUTING_RIPNG_PORT, resp, len, T0 + 10);
    routing_engine_metrics_view(&s_e, T0 + 10, &v);
    TEST_ASSERT_EQUAL_UINT32(1, v.ripng_updates_sent);
    TEST_ASSERT_EQUAL_UINT32(1, v.ripng_requests_answered);
    TEST_ASSERT_EQUAL_UINT32(1, v.ripng_responses_dropped);
    TEST_ASSERT_EQUAL_UINT8(0, v.peer_count);
}
