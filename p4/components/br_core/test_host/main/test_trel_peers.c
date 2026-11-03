#include <string.h>
#include "unity.h"
#include "br_core_trel.h"

static const uint8_t OWN[8] = {0xad, 0xf0, 0xd0, 0x25, 0xc4, 0xb5, 0x1f, 0x00};
static const uint8_t FOREIGN[8] = {0x59, 0x65, 0xb5, 0x7e, 0x30, 0xc0, 0xde, 0xf4};

static br_core_trel_sort_t sort;
static br_core_trel_t out;

static br_core_trel_raw_peer_t peer(uint8_t id, const uint8_t pan[8])
{
    br_core_trel_raw_peer_t p = {.sock_address = "[fe80::1]:12390", .pref_154 = BR_CORE_TREL_PREF_NONE,
                                 .pref_trel = BR_CORE_TREL_PREF_NONE
                                };
    memset(p.ext_address, id, sizeof(p.ext_address));
    memcpy(p.ext_pan_id, pan, sizeof(p.ext_pan_id));
    return p;
}

static void sort_one(const uint8_t *own, br_core_trel_raw_peer_t p)
{
    memset(&out, 0, sizeof(out));
    br_core_trel_sort_init(&sort, own, false);
    br_core_trel_sort_add(&sort, &p);
    br_core_trel_sort_finish(&sort, &out);
}

TEST_CASE("On a peer with this network's ext PAN ID: same_network is true", "[trel_peers]")
{
    sort_one(OWN, peer(1, OWN));
    TEST_ASSERT_EQUAL(1, out.peer_count);
    TEST_ASSERT_TRUE(out.peer_list[0].same_network);
    TEST_ASSERT_EQUAL_STRING("adf0d025c4b51f00", out.peer_list[0].ext_pan_id);
    TEST_ASSERT_EQUAL_STRING("0101010101010101", out.peer_list[0].ext_address);
    TEST_ASSERT_EQUAL_STRING("[fe80::1]:12390", out.peer_list[0].sock_address);
}

TEST_CASE("On a peer with this network's ext PAN ID: counts in peers, not in peers_other", "[trel_peers]")
{
    sort_one(OWN, peer(1, OWN));
    TEST_ASSERT_EQUAL(1, out.peers);
    TEST_ASSERT_EQUAL(0, out.peers_other);
}

TEST_CASE("On a peer with another ext PAN ID: same_network is false", "[trel_peers]")
{
    sort_one(OWN, peer(1, FOREIGN));
    TEST_ASSERT_FALSE(out.peer_list[0].same_network);
    TEST_ASSERT_EQUAL_STRING("5965b57e30c0def4", out.peer_list[0].ext_pan_id);
}

TEST_CASE("On a peer with another ext PAN ID: counts in peers_other", "[trel_peers]")
{
    sort_one(OWN, peer(1, FOREIGN));
    TEST_ASSERT_EQUAL(0, out.peers);
    TEST_ASSERT_EQUAL(1, out.peers_other);
}

TEST_CASE("On a board without an ext PAN ID: every peer is same_network false", "[trel_peers]")
{
    sort_one(NULL, peer(1, OWN));
    TEST_ASSERT_FALSE(out.peer_list[0].same_network);
    TEST_ASSERT_EQUAL(0, out.peers);
    TEST_ASSERT_EQUAL(1, out.peers_other);
}

TEST_CASE("On a peer that is a Thread neighbour: preference carries both radios, none for an unused one", "[trel_peers]")
{
    br_core_trel_raw_peer_t p = peer(1, OWN);

    p.neighbor = true;
    p.pref_trel = 255;
    sort_one(OWN, p);
    TEST_ASSERT_TRUE(out.peer_list[0].neighbor);
    TEST_ASSERT_EQUAL(255, out.peer_list[0].pref_trel);
    TEST_ASSERT_EQUAL(BR_CORE_TREL_PREF_NONE, out.peer_list[0].pref_154);
}

TEST_CASE("On a peer that is not a neighbour: preference is absent", "[trel_peers]")
{
    br_core_trel_raw_peer_t p = peer(1, FOREIGN);

    p.pref_trel = 200; // stale values from the caller are not reported
    sort_one(OWN, p);
    TEST_ASSERT_FALSE(out.peer_list[0].neighbor);
    TEST_ASSERT_EQUAL(BR_CORE_TREL_PREF_NONE, out.peer_list[0].pref_trel);
    TEST_ASSERT_EQUAL(BR_CORE_TREL_PREF_NONE, out.peer_list[0].pref_154);
}

TEST_CASE("On more than 8 peers, foreign ones first: this network's peers lead a list of at most 8", "[trel_peers]")
{
    br_core_trel_sort_init(&sort, OWN, false);
    for (uint8_t i = 0; i < 10; i++) {
        br_core_trel_raw_peer_t p = peer(0x10 + i, FOREIGN);
        br_core_trel_sort_add(&sort, &p);
    }
    for (uint8_t i = 0; i < 3; i++) {
        br_core_trel_raw_peer_t p = peer(0x20 + i, OWN);
        br_core_trel_sort_add(&sort, &p);
    }
    memset(&out, 0, sizeof(out));
    br_core_trel_sort_finish(&sort, &out);
    TEST_ASSERT_EQUAL(BR_CORE_TREL_PEERS_MAX, out.peer_count);
    for (size_t i = 0; i < 3; i++) {
        TEST_ASSERT_TRUE(out.peer_list[i].same_network);
    }
    for (size_t i = 3; i < BR_CORE_TREL_PEERS_MAX; i++) {
        TEST_ASSERT_FALSE(out.peer_list[i].same_network);
    }
}

TEST_CASE("On more than 8 peers: both counts cover the whole table", "[trel_peers]")
{
    br_core_trel_sort_init(&sort, OWN, false);
    for (uint8_t i = 0; i < 10; i++) {
        br_core_trel_raw_peer_t p = peer(0x10 + i, FOREIGN);
        br_core_trel_sort_add(&sort, &p);
    }
    for (uint8_t i = 0; i < 9; i++) {
        br_core_trel_raw_peer_t p = peer(0x30 + i, OWN);
        br_core_trel_sort_add(&sort, &p);
    }
    br_core_trel_sort_finish(&sort, &out);
    TEST_ASSERT_EQUAL(9, out.peers);
    TEST_ASSERT_EQUAL(10, out.peers_other);
}

TEST_CASE("On an empty table: both counts 0, empty list", "[trel_peers]")
{
    memset(&out, 0xff, sizeof(out));
    br_core_trel_sort_init(&sort, OWN, false);
    br_core_trel_sort_finish(&sort, &out);
    TEST_ASSERT_EQUAL(0, out.peers);
    TEST_ASSERT_EQUAL(0, out.peers_other);
    TEST_ASSERT_EQUAL(0, out.peer_count);
}

TEST_CASE("On a count-only sort: counts both networks, lists no peer", "[trel_peers]")
{
    br_core_trel_raw_peer_t mine = peer(1, OWN), theirs = peer(2, FOREIGN);

    memset(&out, 0, sizeof(out));
    br_core_trel_sort_init(&sort, OWN, true);
    br_core_trel_sort_add(&sort, &mine);
    br_core_trel_sort_add(&sort, &theirs);
    br_core_trel_sort_finish(&sort, &out);
    TEST_ASSERT_EQUAL(1, out.peers);
    TEST_ASSERT_EQUAL(1, out.peers_other);
    TEST_ASSERT_EQUAL(0, out.peer_count);
}
