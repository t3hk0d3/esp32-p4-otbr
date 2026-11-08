#include <stdlib.h>
#include <string.h>

#include "rcp_proxy_core.h"
#include "unity.h"
#include "unity_test_runner.h"

#define RING_CAP RCP_PROXY_RING_MIN

static rcp_proxy_addr_t addr(uint8_t last, uint16_t port)
{
    rcp_proxy_addr_t a = {.ip = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 192, 168, 1, last}, .port = port};

    return a;
}

/* ---- rcp_proxy_host_mdns ---- */

TEST_CASE("On a .local name: returns true and the name without .local", "[rcp_proxy]")
{
    char out[64];

    TEST_ASSERT_TRUE(rcp_proxy_host_mdns("otbr-host.local", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("otbr-host", out);
    TEST_ASSERT_TRUE(rcp_proxy_host_mdns("Pi.LOCAL", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("Pi", out);
}

TEST_CASE("On a literal, a DNS name or a bare .local: returns false", "[rcp_proxy]")
{
    char out[64];

    TEST_ASSERT_FALSE(rcp_proxy_host_mdns("192.168.1.1", out, sizeof(out)));
    TEST_ASSERT_FALSE(rcp_proxy_host_mdns("agent.example.com", out, sizeof(out)));
    TEST_ASSERT_FALSE(rcp_proxy_host_mdns(".local", out, sizeof(out)));
    TEST_ASSERT_FALSE(rcp_proxy_host_mdns("local", out, sizeof(out)));
    TEST_ASSERT_FALSE(rcp_proxy_host_mdns("x.locals", out, sizeof(out)));
}

/* ---- rcp_proxy_backoff_next ---- */

TEST_CASE("On consecutive failures: returns 1, 2, 4, 8, 16, 30, 30 s", "[rcp_proxy]")
{
    const uint32_t expected[] = {1, 2, 4, 8, 16, 30, 30, 30};
    uint32_t prev = 0;

    for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); i++) {
        prev = rcp_proxy_backoff_next(prev, 0);
        TEST_ASSERT_EQUAL_UINT32(expected[i], prev);
    }
}

TEST_CASE("On a failure after a connection that lasted 60 s or more: returns 1 s", "[rcp_proxy]")
{
    TEST_ASSERT_EQUAL_UINT32(1, rcp_proxy_backoff_next(30, 60000));
    TEST_ASSERT_EQUAL_UINT32(1, rcp_proxy_backoff_next(16, 3600000));
}

TEST_CASE("On a failure after a connection shorter than 60 s: keeps doubling", "[rcp_proxy]")
{
    TEST_ASSERT_EQUAL_UINT32(16, rcp_proxy_backoff_next(8, 59999));
    TEST_ASSERT_EQUAL_UINT32(30, rcp_proxy_backoff_next(30, 1000));
}

/* ---- rcp_proxy_refusal (UDP listen: refused sources) ---- */

TEST_CASE("On a source refused within the last second: cached, refused again without a lookup", "[rcp_proxy]")
{
    rcp_proxy_refusal_t r;
    const rcp_proxy_addr_t a = addr(9, 4000);

    rcp_proxy_refusal_init(&r);
    TEST_ASSERT_FALSE(rcp_proxy_refusal_cached(&r, &a, 1000));
    rcp_proxy_refusal_note(&r, &a, "on a mesh address", 1000);
    TEST_ASSERT_TRUE(rcp_proxy_refusal_cached(&r, &a, 1000));
    TEST_ASSERT_TRUE(rcp_proxy_refusal_cached(&r, &a, 1000 + RCP_PROXY_REFUSAL_CACHE_MS - 1));
    TEST_ASSERT_EQUAL_STRING("on a mesh address", r.why);
}

TEST_CASE("On a refused source after the cache time, another source or a clock gone back: not cached", "[rcp_proxy]")
{
    rcp_proxy_refusal_t r;
    const rcp_proxy_addr_t a = addr(9, 4000);
    const rcp_proxy_addr_t b = addr(10, 4000);
    const rcp_proxy_addr_t a_port = addr(9, 4001);

    rcp_proxy_refusal_init(&r);
    rcp_proxy_refusal_note(&r, &a, "x", 1000);
    TEST_ASSERT_FALSE(rcp_proxy_refusal_cached(&r, &a, 1000 + RCP_PROXY_REFUSAL_CACHE_MS));
    TEST_ASSERT_FALSE(rcp_proxy_refusal_cached(&r, &b, 1001));
    TEST_ASSERT_FALSE(rcp_proxy_refusal_cached(&r, &a_port, 1001));
    TEST_ASSERT_FALSE(rcp_proxy_refusal_cached(&r, &a, 999));
}

TEST_CASE("On the first refused datagram: logged at once with a count of 1", "[rcp_proxy]")
{
    rcp_proxy_refusal_t r;

    rcp_proxy_refusal_init(&r);
    TEST_ASSERT_EQUAL_UINT32(1, rcp_proxy_refusal_log(&r, 0));
}

TEST_CASE("On refused datagrams within 10 s of a log line: not logged, counted into the next line", "[rcp_proxy]")
{
    rcp_proxy_refusal_t r;

    rcp_proxy_refusal_init(&r);
    rcp_proxy_refusal_log(&r, 5000);
    for (int i = 0; i < 40; i++) {
        TEST_ASSERT_EQUAL_UINT32(0, rcp_proxy_refusal_log(&r, 5000 + i * 100));
    }
    TEST_ASSERT_EQUAL_UINT32(41, rcp_proxy_refusal_log(&r, 5000 + RCP_PROXY_REFUSAL_LOG_MS));
    TEST_ASSERT_EQUAL_UINT32(0, rcp_proxy_refusal_log(&r, 5001 + RCP_PROXY_REFUSAL_LOG_MS));
}

TEST_CASE("On a clock gone back since the last log line: logged at once", "[rcp_proxy]")
{
    rcp_proxy_refusal_t r;

    rcp_proxy_refusal_init(&r);
    rcp_proxy_refusal_log(&r, 50000);
    TEST_ASSERT_EQUAL_UINT32(1, rcp_proxy_refusal_log(&r, 1000));
}

/* ---- rcp_proxy_udp_lock ---- */

TEST_CASE("On the first datagram: locks to its source", "[rcp_proxy]")
{
    rcp_proxy_udp_lock_t lock;
    const rcp_proxy_addr_t a = addr(10, 5000);

    rcp_proxy_udp_lock_init(&lock);
    TEST_ASSERT_FALSE(lock.locked);
    TEST_ASSERT_EQUAL_INT64(-1, rcp_proxy_udp_lock_deadline(&lock));
    TEST_ASSERT_TRUE(rcp_proxy_udp_lock_offer(&lock, &a, 1000));
    TEST_ASSERT_TRUE(lock.locked);
    TEST_ASSERT_TRUE(rcp_proxy_addr_eq(&a, &lock.peer));
    TEST_ASSERT_EQUAL_INT64(1000 + RCP_PROXY_UDP_SILENCE_MS, rcp_proxy_udp_lock_deadline(&lock));
}

TEST_CASE("On a datagram from the locked source: takes it and moves the deadline", "[rcp_proxy]")
{
    rcp_proxy_udp_lock_t lock;
    const rcp_proxy_addr_t a = addr(10, 5000);

    rcp_proxy_udp_lock_init(&lock);
    rcp_proxy_udp_lock_offer(&lock, &a, 1000);
    TEST_ASSERT_TRUE(rcp_proxy_udp_lock_offer(&lock, &a, 20000));
    TEST_ASSERT_EQUAL_INT64(20000 + RCP_PROXY_UDP_SILENCE_MS, rcp_proxy_udp_lock_deadline(&lock));
    TEST_ASSERT_EQUAL_UINT32(0, lock.refused);
}

TEST_CASE("On a datagram from another source while locked: refuses it, refused + 1", "[rcp_proxy]")
{
    rcp_proxy_udp_lock_t lock;
    const rcp_proxy_addr_t a = addr(10, 5000);
    const rcp_proxy_addr_t other_host = addr(11, 5000);
    const rcp_proxy_addr_t other_port = addr(10, 5001);

    rcp_proxy_udp_lock_init(&lock);
    rcp_proxy_udp_lock_offer(&lock, &a, 1000);
    TEST_ASSERT_FALSE(rcp_proxy_udp_lock_offer(&lock, &other_host, 2000));
    TEST_ASSERT_EQUAL_UINT32(1, lock.refused);
    TEST_ASSERT_FALSE(rcp_proxy_udp_lock_offer(&lock, &other_port, 3000));
    TEST_ASSERT_EQUAL_UINT32(2, lock.refused);
    TEST_ASSERT_TRUE(rcp_proxy_addr_eq(&a, &lock.peer));
    // A refused datagram does not keep the lock alive.
    TEST_ASSERT_EQUAL_INT64(1000 + RCP_PROXY_UDP_SILENCE_MS, rcp_proxy_udp_lock_deadline(&lock));
}

TEST_CASE("On 30 s of silence from the locked source: unlocks; the next source locks", "[rcp_proxy]")
{
    rcp_proxy_udp_lock_t lock;
    const rcp_proxy_addr_t a = addr(10, 5000);
    const rcp_proxy_addr_t b = addr(20, 6000);

    rcp_proxy_udp_lock_init(&lock);
    rcp_proxy_udp_lock_offer(&lock, &a, 1000);
    TEST_ASSERT_FALSE(rcp_proxy_udp_lock_expire(&lock, 1000 + RCP_PROXY_UDP_SILENCE_MS - 1));
    TEST_ASSERT_TRUE(lock.locked);
    TEST_ASSERT_TRUE(rcp_proxy_udp_lock_expire(&lock, 1000 + RCP_PROXY_UDP_SILENCE_MS));
    TEST_ASSERT_FALSE(lock.locked);
    TEST_ASSERT_FALSE(rcp_proxy_udp_lock_expire(&lock, 1000 + 2 * RCP_PROXY_UDP_SILENCE_MS));
    TEST_ASSERT_TRUE(rcp_proxy_udp_lock_offer(&lock, &b, 40000));
    TEST_ASSERT_TRUE(rcp_proxy_addr_eq(&b, &lock.peer));
}

TEST_CASE("On a datagram from another source after 30 s of silence without an expire call: locks to it",
          "[rcp_proxy]")
{
    rcp_proxy_udp_lock_t lock;
    const rcp_proxy_addr_t a = addr(10, 5000);
    const rcp_proxy_addr_t b = addr(20, 6000);

    rcp_proxy_udp_lock_init(&lock);
    rcp_proxy_udp_lock_offer(&lock, &a, 1000);
    TEST_ASSERT_TRUE(rcp_proxy_udp_lock_offer(&lock, &b, 1000 + RCP_PROXY_UDP_SILENCE_MS));
    TEST_ASSERT_TRUE(rcp_proxy_addr_eq(&b, &lock.peer));
    TEST_ASSERT_EQUAL_UINT32(0, lock.refused);
}

TEST_CASE("On a kick: unlocks; the next source locks", "[rcp_proxy]")
{
    rcp_proxy_udp_lock_t lock;
    const rcp_proxy_addr_t a = addr(10, 5000);
    const rcp_proxy_addr_t b = addr(20, 6000);

    rcp_proxy_udp_lock_init(&lock);
    rcp_proxy_udp_lock_offer(&lock, &a, 1000);
    rcp_proxy_udp_lock_kick(&lock);
    TEST_ASSERT_FALSE(lock.locked);
    TEST_ASSERT_EQUAL_INT64(-1, rcp_proxy_udp_lock_deadline(&lock));
    TEST_ASSERT_TRUE(rcp_proxy_udp_lock_offer(&lock, &b, 2000));
    TEST_ASSERT_TRUE(rcp_proxy_addr_eq(&b, &lock.peer));
}

TEST_CASE("On a kick: the same source locks again with its next datagram", "[rcp_proxy]")
{
    rcp_proxy_udp_lock_t lock;
    const rcp_proxy_addr_t a = addr(10, 5000);

    rcp_proxy_udp_lock_init(&lock);
    rcp_proxy_udp_lock_offer(&lock, &a, 1000);
    rcp_proxy_udp_lock_kick(&lock);
    TEST_ASSERT_TRUE(rcp_proxy_udp_lock_offer(&lock, &a, 1001));
}

TEST_CASE("On a cut radio write: unlocks, and the peer's datagrams are refused for RCP_PROXY_UDP_BAR_MS",
          "[rcp_proxy]")
{
    rcp_proxy_udp_lock_t lock;
    const rcp_proxy_addr_t a = addr(10, 5000);

    rcp_proxy_udp_lock_init(&lock);
    rcp_proxy_udp_lock_offer(&lock, &a, 1000);
    rcp_proxy_udp_lock_bar(&lock, 1500);
    TEST_ASSERT_FALSE(lock.locked);
    TEST_ASSERT_EQUAL_INT64(-1, rcp_proxy_udp_lock_deadline(&lock));
    TEST_ASSERT_FALSE(rcp_proxy_udp_lock_offer(&lock, &a, 1500));
    TEST_ASSERT_FALSE(rcp_proxy_udp_lock_offer(&lock, &a, 1500 + RCP_PROXY_UDP_BAR_MS - 1));
    TEST_ASSERT_EQUAL_UINT32(2, lock.refused);
    TEST_ASSERT_FALSE(lock.locked);
}

TEST_CASE("On RCP_PROXY_UDP_BAR_MS after a cut radio write: the peer locks again", "[rcp_proxy]")
{
    rcp_proxy_udp_lock_t lock;
    const rcp_proxy_addr_t a = addr(10, 5000);

    rcp_proxy_udp_lock_init(&lock);
    rcp_proxy_udp_lock_offer(&lock, &a, 1000);
    rcp_proxy_udp_lock_bar(&lock, 1500);
    TEST_ASSERT_TRUE(rcp_proxy_udp_lock_offer(&lock, &a, 1500 + RCP_PROXY_UDP_BAR_MS));
    TEST_ASSERT_TRUE(lock.locked);
    TEST_ASSERT_FALSE(lock.barred);
}

TEST_CASE("On a cut radio write: another source (or the same host on another port) locks at once", "[rcp_proxy]")
{
    rcp_proxy_udp_lock_t lock;
    const rcp_proxy_addr_t a = addr(10, 5000);
    const rcp_proxy_addr_t other_port = addr(10, 5001);
    const rcp_proxy_addr_t b = addr(20, 6000);

    rcp_proxy_udp_lock_init(&lock);
    rcp_proxy_udp_lock_offer(&lock, &a, 1000);
    rcp_proxy_udp_lock_bar(&lock, 1500);
    TEST_ASSERT_TRUE(rcp_proxy_udp_lock_offer(&lock, &other_port, 1600));
    rcp_proxy_udp_lock_kick(&lock);
    TEST_ASSERT_TRUE(rcp_proxy_udp_lock_offer(&lock, &b, 1700));
    TEST_ASSERT_TRUE(rcp_proxy_addr_eq(&b, &lock.peer));
}

TEST_CASE("On a clock that went back after a cut radio write: the bar ends", "[rcp_proxy]")
{
    rcp_proxy_udp_lock_t lock;
    const rcp_proxy_addr_t a = addr(10, 5000);

    rcp_proxy_udp_lock_init(&lock);
    rcp_proxy_udp_lock_offer(&lock, &a, 1000);
    rcp_proxy_udp_lock_bar(&lock, 1500);
    TEST_ASSERT_TRUE(rcp_proxy_udp_lock_offer(&lock, &a, 1400));
}

TEST_CASE("On a cut radio write while unlocked: nothing is barred", "[rcp_proxy]")
{
    rcp_proxy_udp_lock_t lock;
    const rcp_proxy_addr_t a = addr(10, 5000);

    rcp_proxy_udp_lock_init(&lock);
    rcp_proxy_udp_lock_bar(&lock, 1500);
    TEST_ASSERT_FALSE(lock.barred);
    TEST_ASSERT_TRUE(rcp_proxy_udp_lock_offer(&lock, &a, 1500));
}

TEST_CASE("On link-local sources in different zones: they are different sources", "[rcp_proxy]")
{
    rcp_proxy_udp_lock_t lock;
    rcp_proxy_addr_t a = {.ip = {0xfe, 0x80, [15] = 1}, .port = 5000, .scope = 2};
    rcp_proxy_addr_t b = a;

    b.scope = 3;
    rcp_proxy_udp_lock_init(&lock);
    rcp_proxy_udp_lock_offer(&lock, &a, 0);
    TEST_ASSERT_FALSE(rcp_proxy_udp_lock_offer(&lock, &b, 1));
}

/* ---- rcp_proxy_peer_new (the reset-handshake replay) ---- */

TEST_CASE("On the first peer of a socket session: returns true (replay)", "[rcp_proxy]")
{
    rcp_proxy_peer_memo_t m;
    const rcp_proxy_addr_t a = addr(10, 5000);

    rcp_proxy_peer_memo_init(&m);
    TEST_ASSERT_TRUE(rcp_proxy_peer_new(&m, &a));
}

TEST_CASE("On the same source locking again after 30 s of silence: returns false (no replay)", "[rcp_proxy]")
{
    rcp_proxy_peer_memo_t m;
    rcp_proxy_udp_lock_t lock;
    const rcp_proxy_addr_t a = addr(10, 5000);

    rcp_proxy_peer_memo_init(&m);
    rcp_proxy_udp_lock_init(&lock);
    TEST_ASSERT_TRUE(rcp_proxy_udp_lock_offer(&lock, &a, 0));
    TEST_ASSERT_TRUE(rcp_proxy_peer_new(&m, &a));
    TEST_ASSERT_TRUE(rcp_proxy_udp_lock_expire(&lock, RCP_PROXY_UDP_SILENCE_MS));
    TEST_ASSERT_TRUE(rcp_proxy_udp_lock_offer(&lock, &a, RCP_PROXY_UDP_SILENCE_MS + 5000));
    TEST_ASSERT_FALSE(rcp_proxy_peer_new(&m, &a));
}

TEST_CASE("On a different source locking after the previous peer: returns true (replay)", "[rcp_proxy]")
{
    rcp_proxy_peer_memo_t m;
    const rcp_proxy_addr_t a = addr(10, 5000);
    const rcp_proxy_addr_t b = addr(20, 5000);
    const rcp_proxy_addr_t a_other_port = addr(10, 5001);

    rcp_proxy_peer_memo_init(&m);
    rcp_proxy_peer_new(&m, &a);
    TEST_ASSERT_TRUE(rcp_proxy_peer_new(&m, &b));
    TEST_ASSERT_TRUE(rcp_proxy_peer_new(&m, &a)); // differs from the previous peer, b
    TEST_ASSERT_TRUE(rcp_proxy_peer_new(&m, &a_other_port));
}

TEST_CASE("On UDP connect: replays once per socket session; a new session (init) replays again", "[rcp_proxy]")
{
    rcp_proxy_peer_memo_t m;
    const rcp_proxy_addr_t target = addr(30, 6638);

    rcp_proxy_peer_memo_init(&m);
    TEST_ASSERT_TRUE(rcp_proxy_peer_new(&m, &target));
    TEST_ASSERT_FALSE(rcp_proxy_peer_new(&m, &target)); // heard again after a silence
    TEST_ASSERT_FALSE(rcp_proxy_peer_new(&m, &target));
    rcp_proxy_peer_memo_init(&m); // the socket was opened again (back-off, re-attach)
    TEST_ASSERT_TRUE(rcp_proxy_peer_new(&m, &target));
}

TEST_CASE("On TCP accept or connect (init before each): every peer session replays, also the same address",
          "[rcp_proxy]")
{
    rcp_proxy_peer_memo_t m;
    const rcp_proxy_addr_t a = addr(10, 5000);

    for (int i = 0; i < 3; i++) {
        rcp_proxy_peer_memo_init(&m);
        TEST_ASSERT_TRUE(rcp_proxy_peer_new(&m, &a));
    }
}

/* ---- rcp_proxy_lookup (connect target, resolved off the proxy task) ---- */

TEST_CASE("On the answer to the pending ask: it is taken once and the session no longer waits", "[rcp_proxy]")
{
    rcp_proxy_lookup_t l = {0};
    uint32_t seq = 0;
    const uint32_t ask = rcp_proxy_lookup_ask(&l, &seq);

    TEST_ASSERT_NOT_EQUAL(0, ask);
    TEST_ASSERT_TRUE(rcp_proxy_lookup_waiting(&l));
    TEST_ASSERT_TRUE(rcp_proxy_lookup_answer(&l, ask));
    TEST_ASSERT_FALSE(rcp_proxy_lookup_waiting(&l));
    TEST_ASSERT_FALSE(rcp_proxy_lookup_answer(&l, ask));
}

TEST_CASE("On an answer to an earlier ask of the session: it is discarded, the latest still taken", "[rcp_proxy]")
{
    rcp_proxy_lookup_t l = {0};
    uint32_t seq = 0;
    const uint32_t first = rcp_proxy_lookup_ask(&l, &seq);
    const uint32_t second = rcp_proxy_lookup_ask(&l, &seq);

    TEST_ASSERT_NOT_EQUAL(first, second);
    TEST_ASSERT_FALSE(rcp_proxy_lookup_answer(&l, first));
    TEST_ASSERT_TRUE(rcp_proxy_lookup_waiting(&l));
    TEST_ASSERT_TRUE(rcp_proxy_lookup_answer(&l, second));
}

TEST_CASE("On an answer after the session stopped waiting (retry, close, new settings): it is discarded",
          "[rcp_proxy]")
{
    rcp_proxy_lookup_t l = {0};
    uint32_t seq = 0;
    const uint32_t ask = rcp_proxy_lookup_ask(&l, &seq);

    rcp_proxy_lookup_cancel(&l);
    TEST_ASSERT_FALSE(rcp_proxy_lookup_waiting(&l));
    TEST_ASSERT_FALSE(rcp_proxy_lookup_answer(&l, ask));
}

TEST_CASE("On an answer for an ended session arriving at the next session: it is discarded", "[rcp_proxy]")
{
    rcp_proxy_lookup_t old = {0};
    rcp_proxy_lookup_t next = {0};
    uint32_t seq = 0; // shared by every session of the task
    const uint32_t stale = rcp_proxy_lookup_ask(&old, &seq);
    const uint32_t ask = rcp_proxy_lookup_ask(&next, &seq);

    TEST_ASSERT_FALSE(rcp_proxy_lookup_answer(&next, stale));
    TEST_ASSERT_TRUE(rcp_proxy_lookup_answer(&next, ask));
}

TEST_CASE("On the counter wrapping: an ask is never 0, and answer 0 is never taken", "[rcp_proxy]")
{
    rcp_proxy_lookup_t l = {0};
    uint32_t seq = UINT32_MAX;

    TEST_ASSERT_EQUAL_UINT32(1, rcp_proxy_lookup_ask(&l, &seq));
    rcp_proxy_lookup_cancel(&l);
    TEST_ASSERT_FALSE(rcp_proxy_lookup_answer(&l, 0));
}

/* ---- rcp_proxy_accept (TCP listen) ---- */

TEST_CASE("On a connection with no peer that both access rules allow: returns RCP_PROXY_TAKE", "[rcp_proxy]")
{
    TEST_ASSERT_EQUAL(RCP_PROXY_TAKE, rcp_proxy_accept(false, false, true, true));
}

TEST_CASE("On a connection while a peer is connected, or a held client sent bytes: returns RCP_PROXY_REFUSE (first "
          "client wins)", "[rcp_proxy]")
{
    TEST_ASSERT_EQUAL(RCP_PROXY_REFUSE, rcp_proxy_accept(true, false, true, true));
}

TEST_CASE("On a connection while the held client has sent nothing (an idle preconnect): returns RCP_PROXY_REPLACE",
          "[rcp_proxy]")
{
    TEST_ASSERT_EQUAL(RCP_PROXY_REPLACE, rcp_proxy_accept(true, true, true, true));
}

TEST_CASE("On a connection the access rules refuse while the held client is silent: returns RCP_PROXY_REFUSE (the "
          "held client stays)", "[rcp_proxy]")
{
    TEST_ASSERT_EQUAL(RCP_PROXY_REFUSE, rcp_proxy_accept(true, true, false, true));
    TEST_ASSERT_EQUAL(RCP_PROXY_REFUSE, rcp_proxy_accept(true, true, true, false));
}

TEST_CASE("On a connection whose local address the access rules refuse: returns RCP_PROXY_REFUSE", "[rcp_proxy]")
{
    TEST_ASSERT_EQUAL(RCP_PROXY_REFUSE, rcp_proxy_accept(false, false, false, true));
}

TEST_CASE("On a client routed over the Thread mesh to an infrastructure address: returns RCP_PROXY_REFUSE",
          "[rcp_proxy]")
{
    // The local address (mgmt or thread_bb) passes netcfg_socket_allowed; netcfg_peer_allowed refuses the mesh route.
    TEST_ASSERT_EQUAL(RCP_PROXY_REFUSE, rcp_proxy_accept(false, false, true, false));
}

/* ---- rcp_proxy_ring ---- */

TEST_CASE("On pushes within the capacity: take returns the bytes in order, across the wrap point", "[rcp_proxy]")
{
    uint8_t storage[16];
    uint8_t out[16];
    rcp_proxy_ring_t r;
    uint8_t a[12], b[10];

    for (size_t i = 0; i < sizeof(a); i++) {
        a[i] = (uint8_t)(i + 1);
    }
    for (size_t i = 0; i < sizeof(b); i++) {
        b[i] = (uint8_t)(100 + i);
    }
    rcp_proxy_ring_init(&r, storage, sizeof(storage));
    TEST_ASSERT_TRUE(rcp_proxy_ring_push(&r, a, sizeof(a)));
    // Drain 8 so the head sits at 8 and the next push wraps.
    TEST_ASSERT_EQUAL(8, rcp_proxy_ring_take(&r, out, 8));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(a, out, 8);
    TEST_ASSERT_TRUE(rcp_proxy_ring_push(&r, b, sizeof(b)));
    TEST_ASSERT_EQUAL(14, r.len);
    TEST_ASSERT_EQUAL(14, rcp_proxy_ring_take(&r, out, sizeof(out)));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(&a[8], out, 4);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(b, &out[4], 10);
    TEST_ASSERT_EQUAL(0, r.len);
}

TEST_CASE("On a push that would overflow: the ring is unchanged", "[rcp_proxy]")
{
    static uint8_t storage[RING_CAP];
    static uint8_t fill[RING_CAP - 2];
    static uint8_t out[RING_CAP];
    const uint8_t extra[3] = {0xAA, 0xBB, 0xCC};
    rcp_proxy_ring_t r;

    for (size_t i = 0; i < sizeof(fill); i++) {
        fill[i] = (uint8_t)(i * 7 + 1);
    }
    rcp_proxy_ring_init(&r, storage, sizeof(storage));
    TEST_ASSERT_TRUE(rcp_proxy_ring_push(&r, fill, sizeof(fill)));
    TEST_ASSERT_EQUAL(RING_CAP - 2, r.len);

    TEST_ASSERT_FALSE(rcp_proxy_ring_push(&r, extra, sizeof(extra)));
    TEST_ASSERT_EQUAL(RING_CAP - 2, r.len);
    TEST_ASSERT_EQUAL(RING_CAP - 2, rcp_proxy_ring_take(&r, out, sizeof(out)));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(fill, out, sizeof(fill));
}

TEST_CASE("On clear: leaves the ring empty", "[rcp_proxy]")
{
    uint8_t storage[16];
    uint8_t out[16];
    const uint8_t data[5] = {1, 2, 3, 4, 5};
    rcp_proxy_ring_t r;

    rcp_proxy_ring_init(&r, storage, sizeof(storage));
    TEST_ASSERT_TRUE(rcp_proxy_ring_push(&r, data, sizeof(data)));
    rcp_proxy_ring_clear(&r);
    TEST_ASSERT_EQUAL(0, r.len);
    TEST_ASSERT_EQUAL(0, rcp_proxy_ring_take(&r, out, sizeof(out)));
    // The cleared ring is fully usable again.
    TEST_ASSERT_TRUE(rcp_proxy_ring_push(&r, data, sizeof(data)));
    TEST_ASSERT_EQUAL(5, rcp_proxy_ring_take(&r, out, sizeof(out)));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(data, out, 5);
}

TEST_CASE("On peek and drop across the wrap point: gives the head run, then the wrapped part", "[rcp_proxy]")
{
    uint8_t storage[8];
    const uint8_t a[6] = {1, 2, 3, 4, 5, 6};
    const uint8_t b[4] = {7, 8, 9, 10};
    const uint8_t *p;
    rcp_proxy_ring_t r;

    rcp_proxy_ring_init(&r, storage, sizeof(storage));
    TEST_ASSERT_EQUAL(0, rcp_proxy_ring_peek(&r, &p));
    rcp_proxy_ring_push(&r, a, sizeof(a));
    rcp_proxy_ring_drop(&r, 4); // head at 4: 5, 6 left
    rcp_proxy_ring_push(&r, b, sizeof(b)); // 5 6 7 8 | 9 10 wrapped
    TEST_ASSERT_EQUAL(4, rcp_proxy_ring_peek(&r, &p));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(((const uint8_t[]) {
        5, 6, 7, 8
    }), p, 4);
    rcp_proxy_ring_drop(&r, 3);
    TEST_ASSERT_EQUAL(1, rcp_proxy_ring_peek(&r, &p));
    TEST_ASSERT_EQUAL_UINT8(8, p[0]);
    rcp_proxy_ring_drop(&r, 1);
    TEST_ASSERT_EQUAL(2, rcp_proxy_ring_peek(&r, &p));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(((const uint8_t[]) {
        9, 10
    }), p, 2);
    rcp_proxy_ring_drop(&r, 2);
    TEST_ASSERT_EQUAL(0, r.len);
}

/* ---- rcp_proxy_cut (UDP framing hdlc on a byte channel) ---- */

typedef struct {
    uint8_t frames[8][64];
    size_t lens[8];
    size_t n;
} sink_t;

static void sink(void *ctx, const uint8_t *frame, size_t len)
{
    sink_t *s = ctx;

    TEST_ASSERT_TRUE(s->n < 8 && len <= 64);
    memcpy(s->frames[s->n], frame, len);
    s->lens[s->n++] = len;
}

TEST_CASE("On two frames that share a flag: emits each with both flags", "[rcp_proxy]")
{
    uint8_t buf[64];
    rcp_proxy_cut_t c;
    sink_t s = {0};
    const uint8_t in[] = {0x7E, 1, 2, 0x7E, 3, 4, 5, 0x7E};

    rcp_proxy_cut_init(&c, buf, sizeof(buf));
    TEST_ASSERT_EQUAL(2, rcp_proxy_cut_feed(&c, in, sizeof(in), sink, &s));
    TEST_ASSERT_EQUAL(4, s.lens[0]);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(((const uint8_t[]) {
        0x7E, 1, 2, 0x7E
    }), s.frames[0], 4);
    TEST_ASSERT_EQUAL(5, s.lens[1]);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(((const uint8_t[]) {
        0x7E, 3, 4, 5, 0x7E
    }), s.frames[1], 5);
    TEST_ASSERT_EQUAL(1, c.len); // the closing flag waits: it may open the next frame
}

TEST_CASE("On a frame split over several feeds: emits it once complete", "[rcp_proxy]")
{
    uint8_t buf[64];
    rcp_proxy_cut_t c;
    sink_t s = {0};
    const uint8_t part1[] = {0xAA, 0x7E, 1, 2};
    const uint8_t part2[] = {3, 0x7E};

    rcp_proxy_cut_init(&c, buf, sizeof(buf));
    TEST_ASSERT_EQUAL(0, rcp_proxy_cut_feed(&c, part1, sizeof(part1), sink, &s));
    TEST_ASSERT_EQUAL(1, rcp_proxy_cut_feed(&c, part2, sizeof(part2), sink, &s));
    TEST_ASSERT_EQUAL(5, s.lens[0]);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(((const uint8_t[]) {
        0x7E, 1, 2, 3, 0x7E
    }), s.frames[0], 5);
}

TEST_CASE("On a buffer full without a complete frame: drops its bytes, overflows + 1, then cuts the next frame",
          "[rcp_proxy]")
{
    uint8_t buf[8];
    rcp_proxy_cut_t c;
    sink_t s = {0};
    const uint8_t too_long[] = {0x7E, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    const uint8_t next[] = {0x7E, 0x11, 0x7E};

    rcp_proxy_cut_init(&c, buf, sizeof(buf));
    TEST_ASSERT_EQUAL(0, rcp_proxy_cut_feed(&c, too_long, sizeof(too_long), sink, &s));
    TEST_ASSERT_EQUAL_UINT32(1, c.overflows);
    TEST_ASSERT_TRUE(c.len < sizeof(buf));
    TEST_ASSERT_EQUAL(1, rcp_proxy_cut_feed(&c, next, sizeof(next), sink, &s));
    TEST_ASSERT_EQUAL(3, s.lens[0]);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(next, s.frames[0], 3);
}

TEST_CASE("On a buffer that fills exactly on a flag: keeps the flag to open the next frame", "[rcp_proxy]")
{
    uint8_t buf[4];
    rcp_proxy_cut_t c;
    sink_t s = {0};
    const uint8_t in[] = {1, 2, 3, 0x7E, 0x22, 0x7E};

    rcp_proxy_cut_init(&c, buf, sizeof(buf));
    TEST_ASSERT_EQUAL(1, rcp_proxy_cut_feed(&c, in, sizeof(in), sink, &s));
    TEST_ASSERT_EQUAL_UINT32(1, c.overflows);
    TEST_ASSERT_EQUAL(3, s.lens[0]);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(((const uint8_t[]) {
        0x7E, 0x22, 0x7E
    }), s.frames[0], 3);
}

TEST_CASE("On clear: forgets the partial frame", "[rcp_proxy]")
{
    uint8_t buf[16];
    rcp_proxy_cut_t c;
    sink_t s = {0};
    const uint8_t partial[] = {0x7E, 1, 2};
    const uint8_t rest[] = {3, 0x7E};

    rcp_proxy_cut_init(&c, buf, sizeof(buf));
    rcp_proxy_cut_feed(&c, partial, sizeof(partial), sink, &s);
    rcp_proxy_cut_clear(&c);
    TEST_ASSERT_EQUAL(0, rcp_proxy_cut_feed(&c, rest, sizeof(rest), sink, &s));
    TEST_ASSERT_EQUAL(0, s.n);
}

/* ---- rcp_proxy_sniff ---- */

static rcp_proxy_sniff_t sniff_text(const char *t, bool timed_out)
{
    return rcp_proxy_sniff((const uint8_t *)t, strlen(t), timed_out);
}

TEST_CASE("On an HTTP method and a space: refuses", "[rcp_proxy]")
{
    static const char *const lines[] = {
        "GET / HT", "POST / H", "PUT /x H", "DELETE /", "HEAD / H", "OPTIONS ", "PATCH / ", "CONNECT ", "TRACE / ",
    };

    for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]); i++) {
        TEST_ASSERT_EQUAL_MESSAGE(RCP_PROXY_SNIFF_REFUSE, sniff_text(lines[i], false), lines[i]);
    }
}

TEST_CASE("On a method and its space before 8 bytes came: refuses at once", "[rcp_proxy]")
{
    TEST_ASSERT_EQUAL(RCP_PROXY_SNIFF_REFUSE, sniff_text("GET ", false));
    TEST_ASSERT_EQUAL(RCP_PROXY_SNIFF_REFUSE, sniff_text("PUT /", false));
}

TEST_CASE("On a method's prefix before the timeout: waits", "[rcp_proxy]")
{
    TEST_ASSERT_EQUAL(RCP_PROXY_SNIFF_WAIT, sniff_text("", false));
    TEST_ASSERT_EQUAL(RCP_PROXY_SNIFF_WAIT, sniff_text("G", false));
    TEST_ASSERT_EQUAL(RCP_PROXY_SNIFF_WAIT, sniff_text("GET", false));
    TEST_ASSERT_EQUAL(RCP_PROXY_SNIFF_WAIT, sniff_text("OPTION", false));
    TEST_ASSERT_EQUAL(RCP_PROXY_SNIFF_WAIT, sniff_text("P", false)); // POST, PUT, PATCH
}

TEST_CASE("On a method without its space at the timeout: passes", "[rcp_proxy]")
{
    TEST_ASSERT_EQUAL(RCP_PROXY_SNIFF_PASS, sniff_text("GET", true));
    TEST_ASSERT_EQUAL(RCP_PROXY_SNIFF_PASS, sniff_text("OPTIONS", true));
}

TEST_CASE("On no bytes yet, timed out or not: waits for the first bytes", "[rcp_proxy]")
{
    TEST_ASSERT_EQUAL(RCP_PROXY_SNIFF_WAIT, sniff_text("", false));
    TEST_ASSERT_EQUAL(RCP_PROXY_SNIFF_WAIT, sniff_text("", true)); // judged when they come (a preconnect)
}

TEST_CASE("On a method followed by another byte than a space: passes", "[rcp_proxy]")
{
    TEST_ASSERT_EQUAL(RCP_PROXY_SNIFF_PASS, sniff_text("GETX", false));
    TEST_ASSERT_EQUAL(RCP_PROXY_SNIFF_PASS, sniff_text("get / HT", false)); // uppercase only
}

TEST_CASE("On a radio protocol's first byte: passes at once", "[rcp_proxy]")
{
    const uint8_t spinel[] = {0x7E, 0x80, 0x01, 0x00};
    const uint8_t znp[] = {0xFE, 0x00, 0x21, 0x01};
    const uint8_t ash[] = {0x1A, 0xC0, 0x38, 0xBC, 0x7E};
    const uint8_t other[] = {0x11};

    TEST_ASSERT_EQUAL(RCP_PROXY_SNIFF_PASS, rcp_proxy_sniff(spinel, 1, false));
    TEST_ASSERT_EQUAL(RCP_PROXY_SNIFF_PASS, rcp_proxy_sniff(spinel, sizeof(spinel), false));
    TEST_ASSERT_EQUAL(RCP_PROXY_SNIFF_PASS, rcp_proxy_sniff(znp, sizeof(znp), false));
    TEST_ASSERT_EQUAL(RCP_PROXY_SNIFF_PASS, rcp_proxy_sniff(ash, sizeof(ash), false));
    TEST_ASSERT_EQUAL(RCP_PROXY_SNIFF_PASS, rcp_proxy_sniff(other, sizeof(other), false));
}

TEST_CASE("On 8 bytes that are no request line: passes without the timeout", "[rcp_proxy]")
{
    TEST_ASSERT_EQUAL(RCP_PROXY_SNIFF_PASS, sniff_text("OPTIONSX", false));
}

/* ---- rcp_proxy_hold ---- */

static rcp_proxy_hold_act_t hold_text(rcp_proxy_hold_t *h, const char *t, int64_t now_ms)
{
    return rcp_proxy_hold_add(h, (const uint8_t *)t, strlen(t), now_ms);
}

TEST_CASE("Hold: on a radio client's first byte: passes at once, the byte held for the radio", "[rcp_proxy]")
{
    rcp_proxy_hold_t h;
    const uint8_t spinel[] = {0x7E, 0x81};

    rcp_proxy_hold_start(&h);
    TEST_ASSERT_EQUAL(RCP_PROXY_HOLD_PASS, rcp_proxy_hold_add(&h, spinel, sizeof(spinel), 1010));
    TEST_ASSERT_EQUAL(2, h.len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(spinel, h.buf, 2);
    TEST_ASSERT_FALSE(rcp_proxy_hold_silent(&h));
    TEST_ASSERT_EQUAL(-1, h.deadline_ms);
}

TEST_CASE("Hold: on a request line within the window: refuses", "[rcp_proxy]")
{
    rcp_proxy_hold_t h;

    rcp_proxy_hold_start(&h);
    TEST_ASSERT_EQUAL(RCP_PROXY_HOLD_REFUSE, hold_text(&h, "POST / H", 1005));
}

TEST_CASE("Hold: on silence after the accept: no timer, the client is never promoted (it stays held, silent)",
          "[rcp_proxy]")
{
    rcp_proxy_hold_t h;

    rcp_proxy_hold_start(&h);
    TEST_ASSERT_EQUAL(-1, h.deadline_ms);
    TEST_ASSERT_TRUE(rcp_proxy_hold_silent(&h));
    TEST_ASSERT_EQUAL(RCP_PROXY_HOLD_WAIT, rcp_proxy_hold_tick(&h, 1000 + RCP_PROXY_SNIFF_MS));
    TEST_ASSERT_EQUAL(RCP_PROXY_HOLD_WAIT, rcp_proxy_hold_tick(&h, 900000));
    TEST_ASSERT_TRUE(rcp_proxy_hold_silent(&h));
}

TEST_CASE("Hold: on a request line long after the accept (preconnect): refuses", "[rcp_proxy]")
{
    rcp_proxy_hold_t h;

    rcp_proxy_hold_start(&h);
    TEST_ASSERT_EQUAL(RCP_PROXY_HOLD_WAIT, rcp_proxy_hold_tick(&h, 1060));
    TEST_ASSERT_EQUAL(RCP_PROXY_HOLD_WAIT, hold_text(&h, "PO", 5000)); // a split first segment
    TEST_ASSERT_FALSE(rcp_proxy_hold_silent(&h)); // from its first byte, a new connection no longer replaces it
    TEST_ASSERT_EQUAL(RCP_PROXY_HOLD_REFUSE, hold_text(&h, "ST / HTT", 5020));
}

TEST_CASE("Hold: on a radio client's first byte long after the accept: passes (it becomes the peer only now)",
          "[rcp_proxy]")
{
    rcp_proxy_hold_t h;
    const uint8_t spinel[] = {0x7E};

    rcp_proxy_hold_start(&h);
    TEST_ASSERT_EQUAL(RCP_PROXY_HOLD_WAIT, rcp_proxy_hold_tick(&h, 1050));
    TEST_ASSERT_EQUAL(RCP_PROXY_HOLD_PASS, rcp_proxy_hold_add(&h, spinel, 1, 3000));
}

TEST_CASE("Hold: on a method's prefix: the timeout counts from the first byte, not the accept", "[rcp_proxy]")
{
    rcp_proxy_hold_t h;

    rcp_proxy_hold_start(&h);
    TEST_ASSERT_EQUAL(RCP_PROXY_HOLD_WAIT, hold_text(&h, "GET", 1040));
    TEST_ASSERT_EQUAL(1040 + RCP_PROXY_SNIFF_MS, h.deadline_ms);
    TEST_ASSERT_EQUAL(RCP_PROXY_HOLD_WAIT, rcp_proxy_hold_tick(&h, 1040 + RCP_PROXY_SNIFF_MS - 1));
    TEST_ASSERT_EQUAL(RCP_PROXY_HOLD_PASS, rcp_proxy_hold_tick(&h, 1040 + RCP_PROXY_SNIFF_MS));
}

TEST_CASE("Hold: on a method's prefix long after the accept, then its space in time: refuses", "[rcp_proxy]")
{
    rcp_proxy_hold_t h;

    rcp_proxy_hold_start(&h);
    TEST_ASSERT_EQUAL(RCP_PROXY_HOLD_WAIT, hold_text(&h, "GET", 2000));
    TEST_ASSERT_EQUAL(RCP_PROXY_HOLD_WAIT, rcp_proxy_hold_tick(&h, 2000 + RCP_PROXY_SNIFF_MS - 1));
    TEST_ASSERT_EQUAL(RCP_PROXY_HOLD_REFUSE, hold_text(&h, " /", 2000 + RCP_PROXY_SNIFF_MS - 1));
}

TEST_CASE("Hold: reads at most RCP_PROXY_SNIFF_LEN bytes", "[rcp_proxy]")
{
    rcp_proxy_hold_t h;

    rcp_proxy_hold_start(&h);
    TEST_ASSERT_EQUAL(RCP_PROXY_SNIFF_LEN, rcp_proxy_hold_room(&h));
    TEST_ASSERT_EQUAL(RCP_PROXY_HOLD_WAIT, hold_text(&h, "OPT", 1));
    TEST_ASSERT_EQUAL(RCP_PROXY_SNIFF_LEN - 3, rcp_proxy_hold_room(&h));
    TEST_ASSERT_EQUAL(RCP_PROXY_HOLD_PASS, hold_text(&h, "IONSXYZW", 2)); // only "IONSX" is taken
    TEST_ASSERT_EQUAL(RCP_PROXY_SNIFF_LEN, h.len);
    TEST_ASSERT_EQUAL_MEMORY("OPTIONSX", h.buf, RCP_PROXY_SNIFF_LEN);
}

/* ---- rcp_proxy_raw_tx_failed ---- */

TEST_CASE("On a write to a channel not attached: discards, the session goes on", "[rcp_proxy]")
{
    TEST_ASSERT_EQUAL(RCP_PROXY_TX_DISCARD, rcp_proxy_raw_tx_failed(ESP_ERR_INVALID_STATE));
}

TEST_CASE("On a write that failed on the device: ends the peer session", "[rcp_proxy]")
{
    TEST_ASSERT_EQUAL(RCP_PROXY_TX_END, rcp_proxy_raw_tx_failed(ESP_ERR_TIMEOUT));
    TEST_ASSERT_EQUAL(RCP_PROXY_TX_END, rcp_proxy_raw_tx_failed(ESP_FAIL));
    TEST_ASSERT_EQUAL(RCP_PROXY_TX_END, rcp_proxy_raw_tx_failed(ESP_ERR_INVALID_RESPONSE));
}

void app_main(void)
{
    UNITY_BEGIN();
    unity_run_all_tests();
    int failures = (int)UNITY_END();
    exit(failures == 0 ? 0 : 1);
}
