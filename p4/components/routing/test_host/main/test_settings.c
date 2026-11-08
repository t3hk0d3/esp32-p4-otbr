#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "unity.h"
#include "unity_test_runner.h"
#include "routing.h"
#include "ipv6_text.h"

/* A complete PUT body; tests replace one part of it. */
#define BODY_FMT "{\"method\":%s,\"omr\":%s,\"announce_ula\":true,\"bgp\":%s}"
#define METHOD_BGP "\"bgp\""
#define OMR_CUSTOM "{\"mode\":\"custom\",\"prefix\":\"2001:db8:42:100::/64\"}"
#define BGP_ONE_PEER "{\"local_asn\":4200000001,\"router_id\":null,\"hold_time\":90," \
                     "\"peers\":[{\"address\":\"2001:db8:42::1\",\"asn\":4200000001}]}"

static const uint8_t PEER_GLOBAL[16] = {0x20, 0x01, 0x0d, 0xb8, 0x00, 0x42, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01};
static const uint8_t PEER_LINK_LOCAL[16] = {0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0x02, 0x11, 0x22, 0xff, 0xfe, 0x33, 0x44, 0x55};
static const routing_prefix64_t CUSTOM = {{0x20, 0x01, 0x0d, 0xb8, 0x00, 0x42, 0x01, 0x00}};
static const routing_prefix64_t LAN = {{0x2a, 0x06, 0x98, 0x01, 0x00, 0xc7, 0xaa, 0x01}};

static char body[1024];

static const char *make_body(const char *method, const char *omr, const char *bgp)
{
    snprintf(body, sizeof(body), BODY_FMT, method, omr, bgp);
    return body;
}

static const char *bgp_with(const char *fields)
{
    static char bgp[512];

    snprintf(bgp, sizeof(bgp), "{%s}", fields);
    return bgp;
}

/* Parse with the defaults as current and no context; returns the bad field, or NULL on ESP_OK. */
static const char *reject_field(const char *json, const routing_validate_ctx_t *ctx)
{
    routing_settings_t current, out;
    const char *bad = NULL;

    routing_settings_defaults(&current);
    const esp_err_t err = routing_settings_from_json(json, &current, ctx, &out, &bad);

    if (err == ESP_OK) {
        return NULL;
    }
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, err);
    TEST_ASSERT_NOT_NULL(bad);
    return bad;
}

static void assert_rejects(const char *field, const char *json, const routing_validate_ctx_t *ctx)
{
    const char *bad = reject_field(json, ctx);

    TEST_ASSERT_NOT_NULL_MESSAGE(bad, json);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(field, bad, json);
}

static void assert_accepts(const char *json, const routing_validate_ctx_t *ctx)
{
    const char *bad = reject_field(json, ctx);

    TEST_ASSERT_NULL_MESSAGE(bad, bad ? bad : "");
}

static void populated(routing_settings_t *s)
{
    memset(s, 0, sizeof(*s));
    s->method = ROUTING_METHOD_BGP;
    s->omr_mode = ROUTING_OMR_CUSTOM;
    s->omr_prefix = CUSTOM;
    s->announce_ula = true;
    s->local_asn = 4200000001u;
    s->router_id_set = true;
    s->router_id = 0xc0a8140f; /* 192.168.20.15 */
    s->hold_time = 180;
    s->peer_count = 2;
    memcpy(s->peers[0].addr, PEER_GLOBAL, 16);
    s->peers[0].asn = 65010;
    memcpy(s->peers[1].addr, PEER_LINK_LOCAL, 16);
    s->peers[1].asn = 4294967295u;
}

/* ---- routing_settings_defaults ---- */

TEST_CASE("Always: method pd, omr auto, announce_ula false, no peers, hold time 90, router id unset", "[routing]")
{
    routing_settings_t s;

    memset(&s, 0xa5, sizeof(s));
    routing_settings_defaults(&s);
    TEST_ASSERT_EQUAL(ROUTING_METHOD_PD, s.method);
    TEST_ASSERT_EQUAL(ROUTING_OMR_AUTO, s.omr_mode);
    TEST_ASSERT_FALSE(s.announce_ula);
    TEST_ASSERT_EQUAL(0, s.peer_count);
    TEST_ASSERT_EQUAL(90, s.hold_time);
    TEST_ASSERT_FALSE(s.router_id_set);
    TEST_ASSERT_EQUAL_UINT32(0, s.router_id);
}

/* ---- routing_settings_from_json ---- */

TEST_CASE("Valid body: fills every field", "[routing]")
{
    const char *json = "{\"method\":\"bgp\",\"omr\":{\"mode\":\"custom\",\"prefix\":\"2001:db8:42:100::/64\"},"
                       "\"announce_ula\":true,\"bgp\":{\"local_asn\":4200000001,\"router_id\":\"192.168.20.15\","
                       "\"hold_time\":180,\"peers\":[{\"address\":\"2001:db8:42::1\",\"asn\":65010},"
                       "{\"address\":\"fe80::211:22ff:fe33:4455\",\"asn\":4294967295}]},"
                       "\"status\":{\"omr_prefix\":\"x\"}}";
    routing_settings_t current, out, expected;
    const char *bad = NULL;

    routing_settings_defaults(&current);
    populated(&expected);
    TEST_ASSERT_EQUAL(ESP_OK, routing_settings_from_json(json, &current, NULL, &out, &bad));
    TEST_ASSERT_EQUAL(ROUTING_METHOD_BGP, out.method);
    TEST_ASSERT_EQUAL(ROUTING_OMR_CUSTOM, out.omr_mode);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(CUSTOM.b, out.omr_prefix.b, 8);
    TEST_ASSERT_TRUE(out.announce_ula);
    TEST_ASSERT_EQUAL_UINT32(4200000001u, out.local_asn);
    TEST_ASSERT_TRUE(out.router_id_set);
    TEST_ASSERT_EQUAL_HEX32(0xc0a8140f, out.router_id);
    TEST_ASSERT_EQUAL(180, out.hold_time);
    TEST_ASSERT_EQUAL(2, out.peer_count);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(PEER_GLOBAL, out.peers[0].addr, 16);
    TEST_ASSERT_EQUAL_UINT32(65010, out.peers[0].asn);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(PEER_LINK_LOCAL, out.peers[1].addr, 16);
    TEST_ASSERT_EQUAL_UINT32(4294967295u, out.peers[1].asn);
    TEST_ASSERT_EQUAL_MEMORY(&expected, &out, sizeof(out));
}

TEST_CASE("Body without bgp: keeps the current BGP settings", "[routing]")
{
    routing_settings_t current, out;

    populated(&current);
    TEST_ASSERT_EQUAL(ESP_OK, routing_settings_from_json(
                          "{\"method\":\"ripng\",\"omr\":{\"mode\":\"auto\"},\"announce_ula\":false}", &current, NULL,
                          &out, NULL));
    TEST_ASSERT_EQUAL(ROUTING_METHOD_RIPNG, out.method);
    TEST_ASSERT_EQUAL(ROUTING_OMR_AUTO, out.omr_mode);
    TEST_ASSERT_FALSE(out.announce_ula);
    TEST_ASSERT_EQUAL_UINT32(current.local_asn, out.local_asn);
    TEST_ASSERT_TRUE(out.router_id_set);
    TEST_ASSERT_EQUAL_HEX32(current.router_id, out.router_id);
    TEST_ASSERT_EQUAL(current.hold_time, out.hold_time);
    TEST_ASSERT_EQUAL(current.peer_count, out.peer_count);
    TEST_ASSERT_EQUAL_MEMORY(current.peers, out.peers, sizeof(out.peers));
}

TEST_CASE("Body missing method, omr or announce_ula: returns ESP_ERR_INVALID_ARG naming that field", "[routing]")
{
    assert_rejects("method", "{\"omr\":{\"mode\":\"auto\"},\"announce_ula\":false}", NULL);
    assert_rejects("omr", "{\"method\":\"pd\",\"announce_ula\":false}", NULL);
    assert_rejects("announce_ula", "{\"method\":\"pd\",\"omr\":{\"mode\":\"auto\"}}", NULL);
}

TEST_CASE("Unknown method, or omr.mode not auto/custom: returns ESP_ERR_INVALID_ARG with bad_field method / omr.mode",
          "[routing]")
{
    assert_rejects("method", make_body("\"ospf\"", OMR_CUSTOM, BGP_ONE_PEER), NULL);
    assert_rejects("method", make_body("1", OMR_CUSTOM, BGP_ONE_PEER), NULL);
    assert_rejects("omr.mode", make_body(METHOD_BGP, "{\"mode\":\"manual\"}", BGP_ONE_PEER), NULL);
    assert_rejects("omr.mode", make_body(METHOD_BGP, "{}", BGP_ONE_PEER), NULL);
}

TEST_CASE("Custom mode without a prefix, or a prefix that is not a /64, unspecified, link-local or multicast: returns "
          "ESP_ERR_INVALID_ARG with bad_field omr.prefix", "[routing]")
{
    static const char *const bad_prefixes[] = {
        "{\"mode\":\"custom\"}",
        "{\"mode\":\"custom\",\"prefix\":null}",
        "{\"mode\":\"custom\",\"prefix\":\"::/64\"}",
        "{\"mode\":\"custom\",\"prefix\":\"fe80::/64\"}",
        "{\"mode\":\"custom\",\"prefix\":\"ff02::/64\"}",
        "{\"mode\":\"custom\",\"prefix\":\"2001:db8::/48\"}",
        "{\"mode\":\"custom\",\"prefix\":\"2001:db8::\"}",
        "{\"mode\":\"custom\",\"prefix\":\"2001:db8::1/64\"}",
        "{\"mode\":\"custom\",\"prefix\":\"2001:db8:::/64\"}",
        "{\"mode\":\"custom\",\"prefix\":\"2001:db8:0:0:0:0:0:0:0/64\"}",
        "{\"mode\":\"custom\",\"prefix\":\"2001:db8::/64 \"}",
    };

    for (size_t i = 0; i < sizeof(bad_prefixes) / sizeof(bad_prefixes[0]); i++) {
        assert_rejects("omr.prefix", make_body(METHOD_BGP, bad_prefixes[i], BGP_ONE_PEER), NULL);
    }
    assert_accepts(make_body(METHOD_BGP, "{\"mode\":\"auto\",\"prefix\":\"fe80::/64\"}", BGP_ONE_PEER), NULL);
}

TEST_CASE("Custom prefix equal to a backbone on-link prefix: returns ESP_ERR_INVALID_ARG with bad_field omr.prefix",
          "[routing]")
{
    const char *json = make_body(METHOD_BGP, "{\"mode\":\"custom\",\"prefix\":\"2a06:9801:c7:aa01::/64\"}",
                                 BGP_ONE_PEER);
    routing_validate_ctx_t ctx;

    memset(&ctx, 0, sizeof(ctx));
    ctx.onlink[0] = CUSTOM;
    ctx.onlink[1] = LAN;
    ctx.onlink_count = 2;
    assert_rejects("omr.prefix", json, &ctx);
    ctx.onlink_count = 1;
    assert_accepts(json, &ctx);
}

TEST_CASE("ASN 0, 23456 or above 4294967295: bad_field bgp.local_asn / bgp.peers", "[routing]")
{
    static const char *const bad_asns[] = {"0", "23456", "4294967296", "-1", "1.5", "\"65000\""};
    char fields[256];

    for (size_t i = 0; i < sizeof(bad_asns) / sizeof(bad_asns[0]); i++) {
        snprintf(fields, sizeof(fields), "\"local_asn\":%s,\"hold_time\":90,"
                 "\"peers\":[{\"address\":\"2001:db8:42::1\",\"asn\":65010}]", bad_asns[i]);
        assert_rejects("bgp.local_asn", make_body(METHOD_BGP, OMR_CUSTOM, bgp_with(fields)), NULL);
        snprintf(fields, sizeof(fields), "\"local_asn\":65010,\"hold_time\":90,"
                 "\"peers\":[{\"address\":\"2001:db8:42::1\",\"asn\":%s}]", bad_asns[i]);
        assert_rejects("bgp.peers", make_body(METHOD_BGP, OMR_CUSTOM, bgp_with(fields)), NULL);
    }
    assert_accepts(make_body(METHOD_BGP, OMR_CUSTOM, bgp_with("\"local_asn\":1,\"hold_time\":90,"
                                                              "\"peers\":[{\"address\":\"2001:db8:42::1\",\"asn\":4294967295}]")), NULL);
}

TEST_CASE("router_id 0.0.0.0 or not dotted IPv4: bad_field bgp.router_id", "[routing]")
{
    static const char *const bad_ids[] = {"\"0.0.0.0\"", "\"192.168.1\"", "\"192.168.1.256\"", "\"192.168.01.1\"",
                                          "\"1.2.3.4 \"", "\"2001:db8::1\"", "3232235777"
                                         };
    char fields[256];

    for (size_t i = 0; i < sizeof(bad_ids) / sizeof(bad_ids[0]); i++) {
        snprintf(fields, sizeof(fields), "\"local_asn\":65010,\"router_id\":%s,\"hold_time\":90,"
                 "\"peers\":[{\"address\":\"2001:db8:42::1\",\"asn\":65010}]", bad_ids[i]);
        assert_rejects("bgp.router_id", make_body(METHOD_BGP, OMR_CUSTOM, bgp_with(fields)), NULL);
    }
}

TEST_CASE("hold_time 1, 2 or above 65535: bad_field bgp.hold_time", "[routing]")
{
    static const char *const bad_holds[] = {"1", "2", "65536", "-3", "90.5", "null"};
    static const char *const good_holds[] = {"0", "3", "65535"};
    char fields[256];

    for (size_t i = 0; i < sizeof(bad_holds) / sizeof(bad_holds[0]); i++) {
        snprintf(fields, sizeof(fields), "\"local_asn\":65010,\"hold_time\":%s,"
                 "\"peers\":[{\"address\":\"2001:db8:42::1\",\"asn\":65010}]", bad_holds[i]);
        assert_rejects("bgp.hold_time", make_body(METHOD_BGP, OMR_CUSTOM, bgp_with(fields)), NULL);
    }
    for (size_t i = 0; i < sizeof(good_holds) / sizeof(good_holds[0]); i++) {
        snprintf(fields, sizeof(fields), "\"local_asn\":65010,\"hold_time\":%s,"
                 "\"peers\":[{\"address\":\"2001:db8:42::1\",\"asn\":65010}]", good_holds[i]);
        assert_accepts(make_body(METHOD_BGP, OMR_CUSTOM, bgp_with(fields)), NULL);
    }
}

TEST_CASE("More than 2 peers, or duplicate peer addresses: bad_field bgp.peers", "[routing]")
{
    assert_rejects("bgp.peers", make_body(METHOD_BGP, OMR_CUSTOM, bgp_with(
                                              "\"local_asn\":65010,\"hold_time\":90,\"peers\":[{\"address\":\"2001:db8:42::1\",\"asn\":65010},"
                                              "{\"address\":\"2001:db8:42::2\",\"asn\":65010},{\"address\":\"2001:db8:42::3\",\"asn\":65010}]")),
                   NULL);
    assert_rejects("bgp.peers", make_body(METHOD_BGP, OMR_CUSTOM, bgp_with(
                                              "\"local_asn\":65010,\"hold_time\":90,\"peers\":[{\"address\":\"2001:db8:42::1\",\"asn\":65010},"
                                              "{\"address\":\"2001:db8:42:0::0:1\",\"asn\":65020}]")), NULL);
}

TEST_CASE("Peer address multicast, unspecified, or one of the board's own addresses: bad_field bgp.peers",
          "[routing]")
{
    static const char *const bad_addrs[] = {"ff02::2", "::", "2001:db8:42::17", "2001:db8:42::1%eth0",
                                            "2001:db8:42::1/128", "not an address"
                                           };
    static const uint8_t own[16] = {0x20, 0x01, 0x0d, 0xb8, 0x00, 0x42, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x17};
    static const uint8_t own_link_local[16] = {0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01};
    routing_validate_ctx_t ctx;
    char fields[256];

    /* Only real addresses in the context, so that "::" is rejected for being unspecified, not for being "own". */
    memset(&ctx, 0, sizeof(ctx));
    memcpy(ctx.own_addrs[0], own_link_local, 16);
    memcpy(ctx.own_addrs[1], own, 16);
    ctx.own_count = 2;
    for (size_t i = 0; i < sizeof(bad_addrs) / sizeof(bad_addrs[0]); i++) {
        snprintf(fields, sizeof(fields), "\"local_asn\":65010,\"hold_time\":90,"
                 "\"peers\":[{\"address\":\"%s\",\"asn\":65010}]", bad_addrs[i]);
        assert_rejects("bgp.peers", make_body(METHOD_BGP, OMR_CUSTOM, bgp_with(fields)), &ctx);
    }
    /* Multicast and unspecified are rejected without a context too. */
    assert_rejects("bgp.peers", make_body(METHOD_BGP, OMR_CUSTOM, bgp_with("\"local_asn\":65010,\"hold_time\":90,"
                                                                           "\"peers\":[{\"address\":\"::\",\"asn\":65010}]")), NULL);
    assert_rejects("bgp.peers", make_body(METHOD_BGP, OMR_CUSTOM, bgp_with("\"local_asn\":65010,\"hold_time\":90,"
                                                                           "\"peers\":[{\"address\":\"ff02::2\",\"asn\":65010}]")), NULL);
    /* The own address is not rejected without the context. */
    assert_accepts(make_body(METHOD_BGP, OMR_CUSTOM, bgp_with("\"local_asn\":65010,\"hold_time\":90,"
                                                              "\"peers\":[{\"address\":\"2001:db8:42::17\",\"asn\":65010}]")), NULL);
}

TEST_CASE("Body without bgp, a current peer now one of the board's own addresses: bad_field bgp.peers", "[routing]")
{
    routing_settings_t current, out;
    routing_validate_ctx_t ctx;
    const char *bad = NULL;
    const char *json = "{\"method\":\"bgp\",\"omr\":{\"mode\":\"auto\"},\"announce_ula\":false}";

    populated(&current);
    memset(&ctx, 0, sizeof(ctx));
    memcpy(ctx.own_addrs[0], PEER_LINK_LOCAL, 16);
    ctx.own_count = 1;
    out = current;
    out.method = ROUTING_METHOD_RIPNG;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, routing_settings_from_json(json, &current, &ctx, &out, &bad));
    TEST_ASSERT_EQUAL_STRING("bgp.peers", bad);
    TEST_ASSERT_EQUAL(ROUTING_METHOD_RIPNG, out.method); /* out left unchanged */
}

TEST_CASE("Body without bgp, a current peer multicast or unspecified: bad_field bgp.peers", "[routing]")
{
    static const uint8_t multicast[16] = {0xff, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x02};
    static const uint8_t unspecified[16] = {0};
    const uint8_t *const bad_addrs[] = {multicast, unspecified};
    const char *json = "{\"method\":\"pd\",\"omr\":{\"mode\":\"auto\"},\"announce_ula\":false}";

    for (size_t i = 0; i < sizeof(bad_addrs) / sizeof(bad_addrs[0]); i++) {
        routing_settings_t current, out;
        const char *bad = NULL;

        populated(&current);
        memcpy(current.peers[1].addr, bad_addrs[i], 16);
        TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, routing_settings_from_json(json, &current, NULL, &out, &bad));
        TEST_ASSERT_EQUAL_STRING("bgp.peers", bad);
    }
}

TEST_CASE("Body without bgp, current peers valid in the context: keeps them", "[routing]")
{
    routing_settings_t current, out;
    routing_validate_ctx_t ctx;
    static const uint8_t own[16] = {0x20, 0x01, 0x0d, 0xb8, 0x00, 0x42, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x17};

    populated(&current);
    memset(&ctx, 0, sizeof(ctx));
    memcpy(ctx.own_addrs[0], own, 16);
    ctx.own_count = 1;
    TEST_ASSERT_EQUAL(ESP_OK, routing_settings_from_json(
                          "{\"method\":\"bgp\",\"omr\":{\"mode\":\"auto\"},\"announce_ula\":false}", &current, &ctx,
                          &out, NULL));
    TEST_ASSERT_EQUAL(2, out.peer_count);
    TEST_ASSERT_EQUAL_MEMORY(current.peers, out.peers, sizeof(out.peers));
}

/* lwIP's inet_pton (ip6addr_aton) accepts every one of these; the settings parser must not. */
TEST_CASE("Peer address with a 5-digit group, embedded IPv4, a trailing colon or trailing text: bad_field bgp.peers",
          "[routing]")
{
    static const char *const bad_addrs[] = {"2001:db8::12345", "::ffff:192.168.1.1",
                                            "2001:db8:42::1:", "2001:db8:42::1 ", "2001:db8:42::g"
                                           };
    char fields[256];

    for (size_t i = 0; i < sizeof(bad_addrs) / sizeof(bad_addrs[0]); i++) {
        snprintf(fields, sizeof(fields), "\"local_asn\":65010,\"hold_time\":90,"
                 "\"peers\":[{\"address\":\"%s\",\"asn\":65010}]", bad_addrs[i]);
        assert_rejects("bgp.peers", make_body(METHOD_BGP, OMR_CUSTOM, bgp_with(fields)), NULL);
    }
    assert_accepts(make_body(METHOD_BGP, OMR_CUSTOM, bgp_with("\"local_asn\":65010,\"hold_time\":90,"
                                                              "\"peers\":[{\"address\":\"2001:DB8:42::1\",\"asn\":65010}]")), NULL);
}

TEST_CASE("Method bgp with no peers: bad_field bgp.peers", "[routing]")
{
    const char *no_peers = bgp_with("\"local_asn\":65010,\"hold_time\":90,\"peers\":[]");

    assert_rejects("bgp.peers", make_body(METHOD_BGP, OMR_CUSTOM, no_peers), NULL);
    assert_rejects("bgp.peers", "{\"method\":\"bgp\",\"omr\":{\"mode\":\"auto\"},\"announce_ula\":false}", NULL);
    assert_accepts(make_body("\"ripng\"", OMR_CUSTOM, no_peers), NULL);
}

TEST_CASE("Text that is not JSON, or trailing text: returns ESP_ERR_INVALID_ARG with bad_field body", "[routing]")
{
    assert_rejects("body", "method=bgp", NULL);
    assert_rejects("body", "[]", NULL);
    assert_rejects("body", "", NULL);
    assert_rejects("body", NULL, NULL);
    snprintf(body, sizeof(body), BODY_FMT "x", METHOD_BGP, OMR_CUSTOM, BGP_ONE_PEER);
    assert_rejects("body", body, NULL);
}

/* ---- routing_settings_from_stored ---- */

TEST_CASE("Stored text missing: returns defaults", "[routing]")
{
    routing_settings_t out, defaults;

    routing_settings_defaults(&defaults);
    memset(&out, 0xa5, sizeof(out));
    TEST_ASSERT_TRUE(routing_settings_from_stored(NULL, &out, NULL));
    TEST_ASSERT_EQUAL_MEMORY(&defaults, &out, sizeof(out));
}

TEST_CASE("Stored text does not parse: returns defaults", "[routing]")
{
    static const char *const bad_texts[] = {"{\"method\":\"bgp\"", "{\"method\":\"ospf\",\"omr\":{\"mode\":\"auto\"},"
                                            "\"announce_ula\":false}", ""
                                           };
    routing_settings_t out, defaults;
    const char *bad = NULL;

    routing_settings_defaults(&defaults);
    for (size_t i = 0; i < sizeof(bad_texts) / sizeof(bad_texts[0]); i++) {
        memset(&out, 0xa5, sizeof(out));
        bad = NULL;
        TEST_ASSERT_FALSE(routing_settings_from_stored(bad_texts[i], &out, &bad));
        TEST_ASSERT_NOT_NULL(bad);
        TEST_ASSERT_EQUAL_MEMORY(&defaults, &out, sizeof(out));
    }
}

TEST_CASE("Stored text valid: returns it unchanged", "[routing]")
{
    routing_settings_t s, out;
    char *json;

    populated(&s);
    json = routing_settings_to_json(&s, NULL);
    TEST_ASSERT_NOT_NULL(json);
    memset(&out, 0xa5, sizeof(out));
    TEST_ASSERT_TRUE(routing_settings_from_stored(json, &out, NULL));
    free(json);
    TEST_ASSERT_EQUAL_MEMORY(&s, &out, sizeof(s));
}

/* ---- routing_settings_to_json ---- */

TEST_CASE("Settings and status: renders the GET shape of spec section 6; status.bgp null unless bgp, status.ripng null "
          "unless ripng", "[routing]")
{
    routing_settings_t s;
    routing_status_t st;
    char *json;

    routing_settings_defaults(&s);
    s.method = ROUTING_METHOD_BGP;
    s.omr_mode = ROUTING_OMR_CUSTOM;
    s.omr_prefix = CUSTOM;
    s.announce_ula = true;
    s.local_asn = 4200000001u;
    s.peer_count = 1;
    memcpy(s.peers[0].addr, PEER_GLOBAL, 16);
    s.peers[0].asn = 4200000001u;
    memset(&st, 0, sizeof(st));
    st.omr_prefix_known = true;
    st.omr_prefix = CUSTOM;
    st.own_omr_prefix_known = true;
    st.own_omr_prefix = CUSTOM;
    st.ula_prefix_known = true;
    st.ula_prefix = (routing_prefix64_t) {
        {
            0xfd, 0x98, 0x26, 0x56, 0x47, 0x40, 0x00, 0x01
        }
    };
    st.announced.p[0] = CUSTOM;
    st.announced.p[1] = st.ula_prefix;
    st.announced.count = 2;
    st.bgp_router_id = 0xc0a8140f;
    st.peer_count = 1;
    memcpy(st.peers[0].addr, PEER_GLOBAL, 16);
    st.peers[0].state = ROUTING_BGP_ESTABLISHED;
    st.peers[0].uptime_s = 8123;
    st.peers[0].received_dropped = 3;
    st.ripng.last_update_ms_ago = 12500;
    st.ripng.updates_sent = 7;
    st.ripng.requests_answered = 2;
    st.ripng.responses_dropped = 1;

    json = routing_settings_to_json(&s, &st);
    TEST_ASSERT_EQUAL_STRING(
        "{\"method\":\"bgp\",\"omr\":{\"mode\":\"custom\",\"prefix\":\"2001:db8:42:100::/64\"},\"announce_ula\":true,"
        "\"bgp\":{\"local_asn\":4200000001,\"router_id\":null,\"hold_time\":90,"
        "\"peers\":[{\"address\":\"2001:db8:42::1\",\"asn\":4200000001}]},"
        "\"status\":{\"omr_prefix\":\"2001:db8:42:100::/64\",\"own_omr_prefix\":\"2001:db8:42:100::/64\","
        "\"ula_prefix\":\"fd98:2656:4740:1::/64\","
        "\"announced\":[\"2001:db8:42:100::/64\",\"fd98:2656:4740:1::/64\"],"
        "\"bgp\":{\"router_id\":\"192.168.20.15\",\"blocked\":null,\"peers\":[{\"address\":\"2001:db8:42::1\","
        "\"state\":\"established\",\"uptime_s\":8123,\"last_error\":null,\"received_dropped\":3}]},"
        "\"ripng\":null}}", json);
    free(json);

    s.method = ROUTING_METHOD_RIPNG;
    s.omr_mode = ROUTING_OMR_AUTO;
    memset(&s.omr_prefix, 0, sizeof(s.omr_prefix));
    s.router_id_set = true;
    s.router_id = 0x0a000001;
    st.omr_prefix_known = false;
    st.own_omr_prefix = st.ula_prefix;
    st.announced.count = 1;
    st.announced.p[0] = st.ula_prefix;
    json = routing_settings_to_json(&s, &st);
    TEST_ASSERT_EQUAL_STRING(
        "{\"method\":\"ripng\",\"omr\":{\"mode\":\"auto\",\"prefix\":null},\"announce_ula\":true,"
        "\"bgp\":{\"local_asn\":4200000001,\"router_id\":\"10.0.0.1\",\"hold_time\":90,"
        "\"peers\":[{\"address\":\"2001:db8:42::1\",\"asn\":4200000001}]},"
        "\"status\":{\"omr_prefix\":null,\"own_omr_prefix\":\"fd98:2656:4740:1::/64\","
        "\"ula_prefix\":\"fd98:2656:4740:1::/64\","
        "\"announced\":[\"fd98:2656:4740:1::/64\"],\"bgp\":null,"
        "\"ripng\":{\"last_update_s_ago\":12,\"updates_sent\":7,\"requests_answered\":2,\"responses_dropped\":1}}}",
        json);
    free(json);

    s.method = ROUTING_METHOD_PD;
    st.ripng.last_update_ms_ago = -1;
    st.announced.count = 0;
    json = routing_settings_to_json(&s, &st);
    TEST_ASSERT_NOT_NULL(strstr(json, "\"announced\":[],\"bgp\":null,\"ripng\":null}}"));
    free(json);

    json = routing_settings_to_json(&s, NULL);
    TEST_ASSERT_NOT_NULL(strstr(json, "\"status\":null}"));
    free(json);
}

/* Settings with PEER_GLOBAL then PEER_LINK_LOCAL, method bgp. */
static void two_peer_settings(routing_settings_t *s)
{
    populated(s);
    s->omr_mode = ROUTING_OMR_AUTO;
    memset(&s->omr_prefix, 0, sizeof(s->omr_prefix));
}

static void status_peer(routing_status_t *st, const uint8_t addr[16], routing_bgp_state_t state, uint32_t uptime_s,
                        const char *last_error)
{
    routing_bgp_peer_status_t *p = &st->peers[st->peer_count++];

    memcpy(p->addr, addr, 16);
    p->state = state;
    p->uptime_s = uptime_s;
    snprintf(p->last_error, sizeof(p->last_error), "%s", last_error);
}

TEST_CASE("Status built from settings with the peers in another order: each peer gets the entry with its address",
          "[routing]")
{
    routing_settings_t s;
    routing_status_t st;

    two_peer_settings(&s);
    memset(&st, 0, sizeof(st));
    status_peer(&st, PEER_LINK_LOCAL, ROUTING_BGP_ACTIVE, 0, "connect: refused");
    status_peer(&st, PEER_GLOBAL, ROUTING_BGP_ESTABLISHED, 42, "");
    char *json = routing_settings_to_json(&s, &st);

    TEST_ASSERT_NOT_NULL(strstr(json, "\"peers\":[{\"address\":\"2001:db8:42::1\",\"state\":\"established\","
                                "\"uptime_s\":42,\"last_error\":null,\"received_dropped\":0},"
                                "{\"address\":\"fe80::211:22ff:fe33:4455\",\"state\":\"active\",\"uptime_s\":0,"
                                "\"last_error\":\"connect: refused\",\"received_dropped\":0}]"));
    free(json);
}

TEST_CASE("Peer the status does not know yet: rendered idle, uptime 0, no last error, nothing dropped", "[routing]")
{
    routing_settings_t s;
    routing_status_t st;

    two_peer_settings(&s);
    memset(&st, 0, sizeof(st));
    status_peer(&st, PEER_GLOBAL, ROUTING_BGP_ESTABLISHED, 42, "");
    status_peer(&st, (const uint8_t[16]) {
        0x20, 0x01, 0x0d, 0xb8, 0x00, 0x42, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x09
    }, ROUTING_BGP_OPENSENT, 7, "x");
    st.peers[1].received_dropped = 5;
    char *json = routing_settings_to_json(&s, &st);

    TEST_ASSERT_NOT_NULL(strstr(json, "{\"address\":\"fe80::211:22ff:fe33:4455\",\"state\":\"idle\",\"uptime_s\":0,"
                                "\"last_error\":null,\"received_dropped\":0}]"));
    free(json);
}

TEST_CASE("Status peer lookup: the entry with that address, NULL when the status has none", "[routing]")
{
    routing_status_t st;

    memset(&st, 0, sizeof(st));
    status_peer(&st, PEER_LINK_LOCAL, ROUTING_BGP_ACTIVE, 0, "");
    TEST_ASSERT_EQUAL_PTR(&st.peers[0], routing_status_peer(&st, PEER_LINK_LOCAL));
    TEST_ASSERT_NULL(routing_status_peer(&st, PEER_GLOBAL));
    memcpy(st.peers[1].addr, PEER_GLOBAL, 16); /* past peer_count: not an entry */
    TEST_ASSERT_NULL(routing_status_peer(&st, PEER_GLOBAL));
}

TEST_CASE("Round trip: from_json(to_json(s)) equals s", "[routing]")
{
    routing_settings_t s, back, current;
    char *json;

    populated(&s);
    routing_settings_defaults(&current);
    json = routing_settings_to_json(&s, NULL);
    TEST_ASSERT_NOT_NULL(json);
    TEST_ASSERT_EQUAL(ESP_OK, routing_settings_from_json(json, &current, NULL, &back, NULL));
    free(json);
    TEST_ASSERT_EQUAL_MEMORY(&s, &back, sizeof(s));

    routing_settings_defaults(&s);
    json = routing_settings_to_json(&s, NULL);
    memset(&back, 0xa5, sizeof(back));
    TEST_ASSERT_EQUAL(ESP_OK, routing_settings_from_json(json, &current, NULL, &back, NULL));
    free(json);
    TEST_ASSERT_EQUAL_MEMORY(&s, &back, sizeof(s));
}

/* ---- routing_settings_diff ---- */

TEST_CASE("Only omr or announce_ula changed: reports PREFIXES only (no session reset)", "[routing]")
{
    routing_settings_t a, b;

    populated(&a);
    b = a;
    TEST_ASSERT_EQUAL_UINT32(0, routing_settings_diff(&a, &b));
    b.omr_prefix.b[7] = 0x01;
    TEST_ASSERT_EQUAL_UINT32(ROUTING_CHANGE_PREFIXES, routing_settings_diff(&a, &b));
    b = a;
    b.omr_mode = ROUTING_OMR_AUTO;
    TEST_ASSERT_EQUAL_UINT32(ROUTING_CHANGE_PREFIXES, routing_settings_diff(&a, &b));
    b = a;
    b.announce_ula = false;
    TEST_ASSERT_EQUAL_UINT32(ROUTING_CHANGE_PREFIXES, routing_settings_diff(&a, &b));
}

TEST_CASE("Peers, ASNs, router id or hold time changed: reports BGP_SESSIONS", "[routing]")
{
    routing_settings_t a, b;

    populated(&a);
    b = a;
    b.peer_count = 1;
    TEST_ASSERT_EQUAL_UINT32(ROUTING_CHANGE_BGP_SESSIONS, routing_settings_diff(&a, &b));
    b = a;
    b.peers[1].addr[15] ^= 1;
    TEST_ASSERT_EQUAL_UINT32(ROUTING_CHANGE_BGP_SESSIONS, routing_settings_diff(&a, &b));
    b = a;
    b.peers[0].asn = 65011;
    TEST_ASSERT_EQUAL_UINT32(ROUTING_CHANGE_BGP_SESSIONS, routing_settings_diff(&a, &b));
    b = a;
    b.local_asn = 65000;
    TEST_ASSERT_EQUAL_UINT32(ROUTING_CHANGE_BGP_SESSIONS, routing_settings_diff(&a, &b));
    b = a;
    b.router_id = 0x0a000001;
    TEST_ASSERT_EQUAL_UINT32(ROUTING_CHANGE_BGP_SESSIONS, routing_settings_diff(&a, &b));
    b = a;
    b.router_id_set = false;
    b.router_id = 0;
    TEST_ASSERT_EQUAL_UINT32(ROUTING_CHANGE_BGP_SESSIONS, routing_settings_diff(&a, &b));
    b = a;
    b.hold_time = 90;
    TEST_ASSERT_EQUAL_UINT32(ROUTING_CHANGE_BGP_SESSIONS, routing_settings_diff(&a, &b));
}

TEST_CASE("Method changed: reports METHOD", "[routing]")
{
    routing_settings_t a, b;

    populated(&a);
    b = a;
    b.method = ROUTING_METHOD_RIPNG;
    TEST_ASSERT_EQUAL_UINT32(ROUTING_CHANGE_METHOD, routing_settings_diff(&a, &b));
}

/* routing_ipv6_to_text */

static const char *ipv6_text(const char *hex)
{
    static char text[ROUTING_IPV6_TEXT_MAX];
    uint8_t addr[16];

    for (int i = 0; i < 16; i++) {
        unsigned v;

        sscanf(hex + 2 * i, "%2x", &v);
        addr[i] = (uint8_t)v;
    }
    routing_ipv6_to_text(addr, text, sizeof(text));
    return text;
}

TEST_CASE("Any address: lower case, no leading zeros, the first longest zero run as ::", "[routing]")
{
    TEST_ASSERT_EQUAL_STRING("2001:db8::1", ipv6_text("20010db8000000000000000000000001"));
    TEST_ASSERT_EQUAL_STRING("fe80::211:22ff:fe33:4455", ipv6_text("fe80000000000000021122fffe334455"));
    TEST_ASSERT_EQUAL_STRING("2001:db8:0:1:1:1:1:1", ipv6_text("20010db8000000010001000100010001"));
    TEST_ASSERT_EQUAL_STRING("2001:0:0:1::1", ipv6_text("20010000000000010000000000000001"));
    TEST_ASSERT_EQUAL_STRING("abcd::", ipv6_text("abcd0000000000000000000000000000"));
}

/* lwIP's inet_ntop (ip6addr_ntoa_r) gets these wrong: upper case, the first zero run, ::FFFF:a.b.c.d. */
TEST_CASE("Address with an earlier shorter zero run, hex letters, or IPv4-mapped: RFC 5952 text", "[routing]")
{
    TEST_ASSERT_EQUAL_STRING("2001:0:0:1::1", ipv6_text("20010000000000010000000000000001"));
    TEST_ASSERT_EQUAL_STRING("fd98:2656:4740:1::abcd", ipv6_text("fd98265647400001000000000000abcd"));
    TEST_ASSERT_EQUAL_STRING("::ffff:c0a8:101", ipv6_text("00000000000000000000ffffc0a80101"));
    TEST_ASSERT_EQUAL_STRING("1:0:2::", ipv6_text("00010000000200000000000000000000"));
}

TEST_CASE("The longest address: fits ROUTING_IPV6_TEXT_MAX", "[routing]")
{
    TEST_ASSERT_EQUAL_STRING("ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff", ipv6_text("ffffffffffffffffffffffffffffffff"));
}

void app_main(void)
{
    UNITY_BEGIN();
    unity_run_all_tests();
    int failures = (int)UNITY_END();
    exit(failures == 0 ? 0 : 1);
}
