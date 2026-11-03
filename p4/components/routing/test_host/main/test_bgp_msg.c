#include <string.h>
#include "unity.h"
#include "routing.h"
#include "vectors.h"

/* Values the scapy vectors were built with (tools/routing_vectors.py). */
#define ASN4 4200000001u
#define ASN2 65010u
#define HOLD 90
#define ROUTER_ID 0xc0a80175u /* 192.168.1.117 */

static const routing_bgp_nexthop_t NH_BOTH = {
    .global = {0x20, 0x01, 0x0d, 0xb8, 0x00, 0x42, 0, 0, 0, 0, 0, 0, 0, 0, 0x00, 0x17},
    .has_global = true,
    .link_local = {0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0x82, 0xf1, 0xb2, 0xff, 0xfe, 0xd2, 0xca, 0xfc},
};

static const routing_bgp_nexthop_t NH_LL_ONLY = {
    .global = {0},
    .has_global = false,
    .link_local = {0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0x82, 0xf1, 0xb2, 0xff, 0xfe, 0xd2, 0xca, 0xfc},
};

/* 2001:db8:42:100::/64 and fd98:2656:4740:1::/64 */
static const routing_prefix_set_t PREFIXES = {
    .p = {{{0x20, 0x01, 0x0d, 0xb8, 0x00, 0x42, 0x01, 0x00}}, {{0xfd, 0x98, 0x26, 0x56, 0x47, 0x40, 0x00, 0x01}}},
    .count = 2,
};

/* My AS = AS_TRANS 23456 */
static const uint8_t MY_AS_TRANS[] = {0x5b, 0xa0};

/* 4-octet ASN capability, 4200000001 */
static const uint8_t CAP_AS4_4200000001[] = {0x41, 0x04, 0xfa, 0x56, 0xea, 0x01};

/* Multiprotocol capability, AFI 2 / SAFI 1 */
static const uint8_t CAP_MP_IPV6_UNICAST[] = {0x01, 0x04, 0x00, 0x02, 0x00, 0x01};

/* ORIGIN IGP, empty AS_PATH, LOCAL_PREF 100 */
static const uint8_t IBGP_ATTRS[] = {0x40, 0x01, 0x01, 0x00, 0x40, 0x02, 0x00, 0x40, 0x05, 0x04, 0x00, 0x00, 0x00, 0x64};

/* MP_REACH_NLRI header, AFI 2 / SAFI 1, next-hop length 32 */
static const uint8_t MP_REACH_NH32[] = {0x80, 0x0e, 0x37, 0x00, 0x02, 0x01, 0x20};

#define FILL 0xa5

static uint8_t s_buf[ROUTING_BGP_MAX_MSG];

static void fill_buf(void)
{
    memset(s_buf, FILL, sizeof(s_buf));
}

/* The encoder wrote exactly `expected` and nothing after it. */
static void assert_written(const uint8_t *expected, size_t expected_len, size_t written)
{
    TEST_ASSERT_EQUAL_size_t(expected_len, written);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, s_buf, expected_len);
    TEST_ASSERT_EQUAL_HEX8(FILL, s_buf[expected_len]);
}

static void assert_err(uint8_t code, uint8_t subcode, routing_bgp_err_t err)
{
    TEST_ASSERT_EQUAL_UINT8(code, err.code);
    TEST_ASSERT_EQUAL_UINT8(subcode, err.subcode);
}

static void assert_untouched_from(size_t from)
{
    for (size_t i = from; i < sizeof(s_buf); i++) {
        TEST_ASSERT_EQUAL_HEX8(FILL, s_buf[i]);
    }
}

static routing_bgp_err_t open_err(const uint8_t *msg, size_t len, uint32_t expected_asn)
{
    routing_bgp_open_t out;

    return routing_bgp_decode_open(msg, len, expected_asn, &out);
}

static routing_bgp_err_t count_err(const uint8_t *msg, size_t len)
{
    uint32_t count;

    return routing_bgp_count_update_nlri(msg, len, &count);
}

static routing_bgp_err_t header_err(const uint8_t *buf, size_t len)
{
    uint8_t type;
    uint16_t msg_len;

    return routing_bgp_decode_header(buf, len, &type, &msg_len);
}

/* --- encode_open --- */

TEST_CASE("With a 4-octet local ASN: writes AS_TRANS (23456) in My AS and the real ASN in the 4-octet capability",
          "[routing]")
{
    fill_buf();
    size_t n = routing_bgp_encode_open(s_buf, sizeof(s_buf), ASN4, HOLD, ROUTER_ID);

    assert_written(BGP_OPEN_AS4, sizeof(BGP_OPEN_AS4), n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(MY_AS_TRANS, &s_buf[20], sizeof(MY_AS_TRANS));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(CAP_AS4_4200000001, &s_buf[37], sizeof(CAP_AS4_4200000001));
}

TEST_CASE("With a 4-octet local ASN: includes the Multiprotocol capability for AFI 2 / SAFI 1", "[routing]")
{
    fill_buf();
    size_t n = routing_bgp_encode_open(s_buf, sizeof(s_buf), ASN4, HOLD, ROUTER_ID);

    assert_written(BGP_OPEN_AS4, sizeof(BGP_OPEN_AS4), n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(CAP_MP_IPV6_UNICAST, &s_buf[31], sizeof(CAP_MP_IPV6_UNICAST));
}

TEST_CASE("With a 2-octet local ASN: writes the ASN in My AS and in the 4-octet capability", "[routing]")
{
    fill_buf();
    size_t n = routing_bgp_encode_open(s_buf, sizeof(s_buf), ASN2, HOLD, ROUTER_ID);

    assert_written(BGP_OPEN_AS2, sizeof(BGP_OPEN_AS2), n);
}

/* --- encode_keepalive, encode_notification --- */

TEST_CASE("KEEPALIVE: writes the 19-byte header of type 4", "[routing]")
{
    fill_buf();
    size_t n = routing_bgp_encode_keepalive(s_buf, sizeof(s_buf));

    assert_written(BGP_KEEPALIVE, sizeof(BGP_KEEPALIVE), n);
}

TEST_CASE("NOTIFICATION 6/2: writes code 6 and subcode 2 without data", "[routing]")
{
    fill_buf();
    size_t n = routing_bgp_encode_notification(s_buf, sizeof(s_buf), 6, 2);

    assert_written(BGP_NOTIFICATION_6_2, sizeof(BGP_NOTIFICATION_6_2), n);
}

/* --- encode_update, encode_withdraw --- */

TEST_CASE("Announcing prefixes on iBGP: carries ORIGIN IGP, an empty AS_PATH and LOCAL_PREF 100", "[routing]")
{
    fill_buf();
    size_t n = routing_bgp_encode_update(s_buf, sizeof(s_buf), true, ASN2, &NH_BOTH, &PREFIXES);

    assert_written(BGP_UPDATE_IBGP, sizeof(BGP_UPDATE_IBGP), n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(IBGP_ATTRS, &s_buf[23], sizeof(IBGP_ATTRS));
}

TEST_CASE("Announcing prefixes on iBGP: carries MP_REACH_NLRI with a 32-byte next-hop (global + link-local) and "
          "every prefix", "[routing]")
{
    fill_buf();
    size_t n = routing_bgp_encode_update(s_buf, sizeof(s_buf), true, ASN2, &NH_BOTH, &PREFIXES);

    assert_written(BGP_UPDATE_IBGP, sizeof(BGP_UPDATE_IBGP), n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(MP_REACH_NH32, &s_buf[37], sizeof(MP_REACH_NH32));
}

TEST_CASE("Announcing prefixes on eBGP: carries AS_PATH [local ASN] and no LOCAL_PREF", "[routing]")
{
    fill_buf();
    size_t n = routing_bgp_encode_update(s_buf, sizeof(s_buf), false, ASN4, &NH_BOTH, &PREFIXES);

    assert_written(BGP_UPDATE_EBGP, sizeof(BGP_UPDATE_EBGP), n);
}

TEST_CASE("Without a global address: carries a 16-byte link-local next-hop", "[routing]")
{
    fill_buf();
    size_t n = routing_bgp_encode_update(s_buf, sizeof(s_buf), true, ASN2, &NH_LL_ONLY, &PREFIXES);

    assert_written(BGP_UPDATE_LL_ONLY, sizeof(BGP_UPDATE_LL_ONLY), n);
}

TEST_CASE("Withdrawing prefixes: carries MP_UNREACH_NLRI only, with no path attributes besides it", "[routing]")
{
    fill_buf();
    size_t n = routing_bgp_encode_withdraw(s_buf, sizeof(s_buf), &PREFIXES);

    assert_written(BGP_WITHDRAW, sizeof(BGP_WITHDRAW), n);
}

TEST_CASE("Empty announce set: returns 0 and writes nothing", "[routing]")
{
    routing_prefix_set_t empty = PREFIXES;

    empty.count = 0;
    fill_buf();
    TEST_ASSERT_EQUAL_size_t(0, routing_bgp_encode_update(s_buf, sizeof(s_buf), true, ASN2, &NH_BOTH, &empty));
    assert_untouched_from(0);
}

TEST_CASE("More prefixes than ROUTING_MAX_ANNOUNCED: returns 0 and writes nothing", "[routing]")
{
    routing_prefix_set_t too_many = PREFIXES;

    too_many.count = ROUTING_MAX_ANNOUNCED + 1;
    fill_buf();
    TEST_ASSERT_EQUAL_size_t(0, routing_bgp_encode_update(s_buf, sizeof(s_buf), true, ASN2, &NH_BOTH, &too_many));
    TEST_ASSERT_EQUAL_size_t(0, routing_bgp_encode_withdraw(s_buf, sizeof(s_buf), &too_many));
    assert_untouched_from(0);
}

/* --- encoders, buffer too small --- */

TEST_CASE("Buffer too small: return 0 and write nothing past cap", "[routing]")
{
    size_t cap;

    fill_buf();
    cap = sizeof(BGP_OPEN_AS4) - 1;
    TEST_ASSERT_EQUAL_size_t(0, routing_bgp_encode_open(s_buf, cap, ASN4, HOLD, ROUTER_ID));
    assert_untouched_from(cap);

    fill_buf();
    cap = sizeof(BGP_KEEPALIVE) - 1;
    TEST_ASSERT_EQUAL_size_t(0, routing_bgp_encode_keepalive(s_buf, cap));
    assert_untouched_from(cap);

    fill_buf();
    cap = sizeof(BGP_UPDATE_IBGP) - 1;
    TEST_ASSERT_EQUAL_size_t(0, routing_bgp_encode_update(s_buf, cap, true, ASN2, &NH_BOTH, &PREFIXES));
    assert_untouched_from(cap);

    fill_buf();
    cap = sizeof(BGP_WITHDRAW) - 1;
    TEST_ASSERT_EQUAL_size_t(0, routing_bgp_encode_withdraw(s_buf, cap, &PREFIXES));
    assert_untouched_from(cap);

    fill_buf();
    cap = sizeof(BGP_NOTIFICATION_6_2) - 1;
    TEST_ASSERT_EQUAL_size_t(0, routing_bgp_encode_notification(s_buf, cap, 6, 2));
    assert_untouched_from(cap);
}

/* --- decode_header --- */

TEST_CASE("Valid header: returns type and length", "[routing]")
{
    uint8_t type = 0;
    uint16_t msg_len = 0;

    assert_err(0, 0, routing_bgp_decode_header(BGP_UPDATE_IBGP, sizeof(BGP_UPDATE_IBGP), &type, &msg_len));
    TEST_ASSERT_EQUAL_UINT8(ROUTING_BGP_MSG_UPDATE, type);
    TEST_ASSERT_EQUAL_UINT16(sizeof(BGP_UPDATE_IBGP), msg_len);

    /* only the header needs to be there */
    assert_err(0, 0, routing_bgp_decode_header(BGP_KEEPALIVE, ROUTING_BGP_HDR_LEN, &type, &msg_len));
    TEST_ASSERT_EQUAL_UINT8(ROUTING_BGP_MSG_KEEPALIVE, type);
    TEST_ASSERT_EQUAL_UINT16(ROUTING_BGP_HDR_LEN, msg_len);
}

TEST_CASE("Marker not all ones: returns a header error (NOTIFICATION 1/1)", "[routing]")
{
    assert_err(1, 1, header_err(BGP_RX_BAD_MARKER, sizeof(BGP_RX_BAD_MARKER)));
}

TEST_CASE("Length below 19 or above 4096: returns a header error (NOTIFICATION 1/2)", "[routing]")
{
    assert_err(1, 2, header_err(BGP_RX_LEN_18, sizeof(BGP_RX_LEN_18)));
    assert_err(1, 2, header_err(BGP_RX_LEN_4097, sizeof(BGP_RX_LEN_4097)));
}

TEST_CASE("Length wrong for the message type: returns a header error (NOTIFICATION 1/2)", "[routing]")
{
    assert_err(1, 2, header_err(BGP_RX_KEEPALIVE_LEN_20, sizeof(BGP_RX_KEEPALIVE_LEN_20)));
}

TEST_CASE("Fewer than 19 bytes available: returns a header error (NOTIFICATION 1/2)", "[routing]")
{
    assert_err(1, 2, header_err(BGP_KEEPALIVE, ROUTING_BGP_HDR_LEN - 1));
}

TEST_CASE("Unknown message type: returns a header error (NOTIFICATION 1/3)", "[routing]")
{
    assert_err(1, 3, header_err(BGP_RX_TYPE_5, sizeof(BGP_RX_TYPE_5)));
}

/* --- count_update_nlri --- */

TEST_CASE("UPDATE with MP_REACH_NLRI and IPv4 NLRI: returns the number of prefixes in both", "[routing]")
{
    uint32_t count = 0;

    assert_err(0, 0, routing_bgp_count_update_nlri(BGP_RX_UPDATE_5_NLRI, sizeof(BGP_RX_UPDATE_5_NLRI), &count));
    TEST_ASSERT_EQUAL_UINT32(5, count);
}

TEST_CASE("UPDATE with MP_UNREACH_NLRI only: returns 0", "[routing]")
{
    uint32_t count = 99;

    assert_err(0, 0, routing_bgp_count_update_nlri(BGP_WITHDRAW, sizeof(BGP_WITHDRAW), &count));
    TEST_ASSERT_EQUAL_UINT32(0, count);
}

TEST_CASE("Attribute length runs past the message: returns an UPDATE error (NOTIFICATION 3/1)", "[routing]")
{
    assert_err(3, 1, count_err(BGP_RX_UPDATE_ATTR_OVERRUN, sizeof(BGP_RX_UPDATE_ATTR_OVERRUN)));
}

TEST_CASE("Total attribute length runs past the message: returns an UPDATE error (NOTIFICATION 3/1)", "[routing]")
{
    /* the same bytes with the message cut short inside MP_REACH_NLRI */
    assert_err(3, 1, count_err(BGP_RX_UPDATE_5_NLRI, 60));
}

TEST_CASE("Prefix length above 128 inside MP_REACH_NLRI: returns an UPDATE error (NOTIFICATION 3/10)", "[routing]")
{
    assert_err(3, 10, count_err(BGP_RX_UPDATE_PREFIX_129, sizeof(BGP_RX_UPDATE_PREFIX_129)));
}

/* --- decode_open --- */

TEST_CASE("Peer ASN differs from the configured one: returns an OPEN error (NOTIFICATION 2/2)", "[routing]")
{
    assert_err(2, 2, open_err(BGP_OPEN_AS4, sizeof(BGP_OPEN_AS4), ASN4 + 1));
    /* a 2-octet peer ASN, carried in the 4-octet capability */
    assert_err(2, 2, open_err(BGP_RX_OPEN_HOLD0, sizeof(BGP_RX_OPEN_HOLD0), 65021));
    /* AS_TRANS in My AS is not taken for the real ASN */
    assert_err(2, 2, open_err(BGP_OPEN_AS4, sizeof(BGP_OPEN_AS4), 23456));
}

TEST_CASE("Peer offers hold time 1 or 2: returns an OPEN error (NOTIFICATION 2/6)", "[routing]")
{
    assert_err(2, 6, open_err(BGP_RX_OPEN_HOLD1, sizeof(BGP_RX_OPEN_HOLD1), 65020));
    assert_err(2, 6, open_err(BGP_RX_OPEN_HOLD2, sizeof(BGP_RX_OPEN_HOLD2), 65020));
}

TEST_CASE("Peer lacks the 4-octet ASN capability: returns an OPEN error (NOTIFICATION 2/7)", "[routing]")
{
    /* My AS matches the expected ASN, and a different one still gets 2/7, not 2/2 */
    assert_err(2, 7, open_err(BGP_RX_OPEN_NO_AS4, sizeof(BGP_RX_OPEN_NO_AS4), 65020));
    assert_err(2, 7, open_err(BGP_RX_OPEN_NO_AS4, sizeof(BGP_RX_OPEN_NO_AS4), 65021));
}

TEST_CASE("Peer lacks IPv6 unicast: returns an OPEN error (NOTIFICATION 2/7)", "[routing]")
{
    assert_err(2, 7, open_err(BGP_RX_OPEN_NO_IPV6, sizeof(BGP_RX_OPEN_NO_IPV6), 65020));
}

TEST_CASE("Version not 4: returns an OPEN error (NOTIFICATION 2/1)", "[routing]")
{
    assert_err(2, 1, open_err(BGP_RX_OPEN_VERSION3, sizeof(BGP_RX_OPEN_VERSION3), 65020));
}

TEST_CASE("BGP identifier 0: returns an OPEN error (NOTIFICATION 2/3)", "[routing]")
{
    assert_err(2, 3, open_err(BGP_RX_OPEN_ID_ZERO, sizeof(BGP_RX_OPEN_ID_ZERO), 65020));
}

TEST_CASE("Optional parameter other than Capabilities: returns an OPEN error (NOTIFICATION 2/4)", "[routing]")
{
    assert_err(2, 4, open_err(BGP_RX_OPEN_AUTH_PARAM, sizeof(BGP_RX_OPEN_AUTH_PARAM), 65020));
}

TEST_CASE("Optional parameters run past the message: returns an OPEN error (NOTIFICATION 2/0)", "[routing]")
{
    assert_err(2, 0, open_err(BGP_OPEN_AS4, sizeof(BGP_OPEN_AS4) - 1, ASN4));
}

TEST_CASE("OPEN shorter than 29 bytes: returns a header error (NOTIFICATION 1/2)", "[routing]")
{
    assert_err(1, 2, open_err(BGP_OPEN_AS4, 28, ASN4));
}

TEST_CASE("Valid OPEN: returns the peer ASN (from the 4-octet capability), router ID and hold time",
          "[routing]")
{
    routing_bgp_open_t out;

    memset(&out, 0, sizeof(out));
    assert_err(0, 0, routing_bgp_decode_open(BGP_OPEN_AS4, sizeof(BGP_OPEN_AS4), ASN4, &out));
    TEST_ASSERT_EQUAL_UINT32(ASN4, out.asn);
    TEST_ASSERT_EQUAL_HEX32(ROUTER_ID, out.router_id);
    TEST_ASSERT_EQUAL_UINT16(HOLD, out.hold_time);
    TEST_ASSERT_TRUE(out.ipv6_unicast);
}

TEST_CASE("Capabilities across several parameters, one unknown: returns the ASN, router ID and hold time",
          "[routing]")
{
    routing_bgp_open_t out;

    memset(&out, 0, sizeof(out));
    assert_err(0, 0, routing_bgp_decode_open(BGP_RX_OPEN_SPLIT_CAPS, sizeof(BGP_RX_OPEN_SPLIT_CAPS), 65020, &out));
    TEST_ASSERT_EQUAL_UINT32(65020, out.asn);
    TEST_ASSERT_EQUAL_HEX32(0xc0a80101, out.router_id);
    TEST_ASSERT_EQUAL_UINT16(180, out.hold_time);
    TEST_ASSERT_TRUE(out.ipv6_unicast);
}

TEST_CASE("Peer offers hold time 0: is accepted with hold time 0", "[routing]")
{
    routing_bgp_open_t out;

    memset(&out, 0xff, sizeof(out));
    assert_err(0, 0, routing_bgp_decode_open(BGP_RX_OPEN_HOLD0, sizeof(BGP_RX_OPEN_HOLD0), 65020, &out));
    TEST_ASSERT_EQUAL_UINT16(0, out.hold_time);
}

/* --- decode_notification --- */

TEST_CASE("NOTIFICATION 6/2: returns code 6, subcode 2", "[routing]")
{
    uint8_t code = 0;
    uint8_t subcode = 0;

    assert_err(0, 0, routing_bgp_decode_notification(BGP_NOTIFICATION_6_2, sizeof(BGP_NOTIFICATION_6_2), &code,
                                                     &subcode));
    TEST_ASSERT_EQUAL_UINT8(6, code);
    TEST_ASSERT_EQUAL_UINT8(2, subcode);
}
