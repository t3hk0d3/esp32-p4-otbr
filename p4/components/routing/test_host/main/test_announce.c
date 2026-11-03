#include <string.h>
#include "unity.h"
#include "routing.h"

static const routing_prefix64_t CUSTOM = {{0x20, 0x01, 0x0d, 0xb8, 0x00, 0x42, 0x01, 0x00}};
static const routing_prefix64_t ULA = {{0xfd, 0x98, 0x26, 0x56, 0x47, 0x40, 0x00, 0x01}};
static const routing_prefix64_t OTHER = {{0x20, 0x01, 0x0d, 0xb8, 0x00, 0x99, 0x00, 0x00}};

/* Method bgp, custom OMR, ULA ticked and known, both in Network Data, border router up: announces both. */
static routing_announce_input_t full_input(void)
{
    routing_announce_input_t in = {
        .method = ROUTING_METHOD_BGP,
        .omr_mode = ROUTING_OMR_CUSTOM,
        .custom = CUSTOM,
        .announce_ula = true,
        .ula_known = true,
        .ula = ULA,
        .custom_in_netdata = true,
        .ula_in_netdata = true,
        .br_up = true,
    };
    return in;
}

/* Run compute on an output pre-filled with junk, so that a missing reset shows. */
static routing_prefix_set_t compute(const routing_announce_input_t *in)
{
    routing_prefix_set_t out;

    memset(&out, 0xaa, sizeof(out));
    out.count = ROUTING_MAX_ANNOUNCED;
    routing_announce_compute(in, &out);
    return out;
}

static routing_prefix_set_t set_of(uint8_t count, const routing_prefix64_t *a, const routing_prefix64_t *b)
{
    routing_prefix_set_t s;

    memset(&s, 0, sizeof(s));
    s.count = count;
    if (count > 0) {
        s.p[0] = *a;
    }
    if (count > 1) {
        s.p[1] = *b;
    }
    return s;
}

static void assert_prefix(const routing_prefix64_t *expected, const routing_prefix64_t *actual)
{
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected->b, actual->b, sizeof(expected->b));
}

TEST_CASE("Custom OMR, ULA box ticked, both in Network Data, border router running and attached: returns the custom "
          "prefix and the ULA", "[routing]")
{
    routing_announce_input_t in = full_input();
    routing_prefix_set_t out = compute(&in);

    TEST_ASSERT_EQUAL_UINT8(2, out.count);
    assert_prefix(&CUSTOM, &out.p[0]);
    assert_prefix(&ULA, &out.p[1]);

    in.method = ROUTING_METHOD_RIPNG;
    out = compute(&in);
    TEST_ASSERT_EQUAL_UINT8(2, out.count);
    assert_prefix(&CUSTOM, &out.p[0]);
    assert_prefix(&ULA, &out.p[1]);
}

TEST_CASE("Custom OMR, ULA box clear: returns the custom prefix only", "[routing]")
{
    routing_announce_input_t in = full_input();

    in.announce_ula = false;
    routing_prefix_set_t out = compute(&in);
    TEST_ASSERT_EQUAL_UINT8(1, out.count);
    assert_prefix(&CUSTOM, &out.p[0]);
}

TEST_CASE("Auto OMR, ULA box ticked, ULA in Network Data: returns the ULA", "[routing]")
{
    routing_announce_input_t in = full_input();

    in.omr_mode = ROUTING_OMR_AUTO;
    routing_prefix_set_t out = compute(&in);
    TEST_ASSERT_EQUAL_UINT8(1, out.count);
    assert_prefix(&ULA, &out.p[0]);
}

TEST_CASE("Auto OMR, ULA box clear: returns an empty set", "[routing]")
{
    routing_announce_input_t in = full_input();

    in.omr_mode = ROUTING_OMR_AUTO;
    in.announce_ula = false;
    routing_prefix_set_t out = compute(&in);
    TEST_ASSERT_EQUAL_UINT8(0, out.count);
}

TEST_CASE("A selected prefix is not in Network Data: leaves that prefix out", "[routing]")
{
    routing_announce_input_t in = full_input();

    in.custom_in_netdata = false;
    routing_prefix_set_t out = compute(&in);
    TEST_ASSERT_EQUAL_UINT8(1, out.count);
    assert_prefix(&ULA, &out.p[0]);

    in = full_input();
    in.ula_in_netdata = false;
    out = compute(&in);
    TEST_ASSERT_EQUAL_UINT8(1, out.count);
    assert_prefix(&CUSTOM, &out.p[0]);

    /* An unknown ULA is left out too, even when flagged as in Network Data. */
    in = full_input();
    in.ula_known = false;
    out = compute(&in);
    TEST_ASSERT_EQUAL_UINT8(1, out.count);
    assert_prefix(&CUSTOM, &out.p[0]);
}

TEST_CASE("Border router not running or Thread detached: returns an empty set", "[routing]")
{
    routing_announce_input_t in = full_input();

    in.br_up = false;
    routing_prefix_set_t out = compute(&in);
    TEST_ASSERT_EQUAL_UINT8(0, out.count);
}

TEST_CASE("Method pd or off: returns an empty set", "[routing]")
{
    routing_announce_input_t in = full_input();

    in.method = ROUTING_METHOD_PD;
    routing_prefix_set_t out = compute(&in);
    TEST_ASSERT_EQUAL_UINT8(0, out.count);

    in.method = ROUTING_METHOD_OFF;
    out = compute(&in);
    TEST_ASSERT_EQUAL_UINT8(0, out.count);
}

TEST_CASE("Custom prefix equal to the ULA: returns it once", "[routing]")
{
    routing_announce_input_t in = full_input();

    in.ula = CUSTOM;
    routing_prefix_set_t out = compute(&in);
    TEST_ASSERT_EQUAL_UINT8(1, out.count);
    assert_prefix(&CUSTOM, &out.p[0]);
}

TEST_CASE("Set changed: returns the added and removed prefixes separately", "[routing]")
{
    routing_prefix_set_t old_set = set_of(2, &CUSTOM, &ULA);
    routing_prefix_set_t new_set = set_of(2, &ULA, &OTHER);
    routing_prefix_set_t added, removed;

    routing_announce_diff(&old_set, &new_set, &added, &removed);
    TEST_ASSERT_EQUAL_UINT8(1, added.count);
    assert_prefix(&OTHER, &added.p[0]);
    TEST_ASSERT_EQUAL_UINT8(1, removed.count);
    assert_prefix(&CUSTOM, &removed.p[0]);

    /* From empty to two, and back. */
    routing_prefix_set_t empty = set_of(0, NULL, NULL);
    routing_announce_diff(&empty, &old_set, &added, &removed);
    TEST_ASSERT_EQUAL_UINT8(2, added.count);
    assert_prefix(&CUSTOM, &added.p[0]);
    assert_prefix(&ULA, &added.p[1]);
    TEST_ASSERT_EQUAL_UINT8(0, removed.count);

    routing_announce_diff(&old_set, &empty, &added, &removed);
    TEST_ASSERT_EQUAL_UINT8(0, added.count);
    TEST_ASSERT_EQUAL_UINT8(2, removed.count);
    assert_prefix(&CUSTOM, &removed.p[0]);
    assert_prefix(&ULA, &removed.p[1]);
}

TEST_CASE("Set unchanged: returns empty added and removed sets", "[routing]")
{
    routing_prefix_set_t old_set = set_of(2, &CUSTOM, &ULA);
    routing_prefix_set_t new_set = set_of(2, &ULA, &CUSTOM); /* same set, other order */
    routing_prefix_set_t added = set_of(1, &OTHER, NULL);
    routing_prefix_set_t removed = set_of(1, &OTHER, NULL);

    routing_announce_diff(&old_set, &new_set, &added, &removed);
    TEST_ASSERT_EQUAL_UINT8(0, added.count);
    TEST_ASSERT_EQUAL_UINT8(0, removed.count);
}
