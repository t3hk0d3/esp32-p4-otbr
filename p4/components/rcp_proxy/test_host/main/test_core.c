#include <stdlib.h>
#include <string.h>

#include "rcp_proxy_core.h"
#include "unity.h"
#include "unity_test_runner.h"

#define RING_CAP RCP_PROXY_RING_MIN

static radio_mux_proxy_settings_t defaults(void)
{
    radio_mux_proxy_settings_t s;

    radio_mux_proxy_defaults(&s);
    return s;
}

static radio_mux_proxy_settings_t connect_to(const char *host)
{
    radio_mux_proxy_settings_t s = defaults();

    s.dir = RADIO_MUX_PROXY_CONNECT;
    strncpy(s.host, host, sizeof(s.host) - 1);
    return s;
}

static rcp_proxy_addr_t addr(uint8_t last, uint16_t port)
{
    rcp_proxy_addr_t a = {.ip = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 192, 168, 1, last}, .port = port};

    return a;
}

/* ---- rcp_proxy_settings_check ---- */

TEST_CASE("On the defaults (listen, tcp, 6638): returns RCP_PROXY_OK", "[rcp_proxy]")
{
    radio_mux_proxy_settings_t s = defaults();

    TEST_ASSERT_EQUAL(RCP_PROXY_OK, rcp_proxy_settings_check(&s));
}

TEST_CASE("On connect without a host: returns RCP_PROXY_NEED_HOST", "[rcp_proxy]")
{
    radio_mux_proxy_settings_t s = connect_to("");

    TEST_ASSERT_EQUAL(RCP_PROXY_NEED_HOST, rcp_proxy_settings_check(&s));
    s.adapter = RADIO_MUX_PROXY_UDP;
    TEST_ASSERT_EQUAL(RCP_PROXY_NEED_HOST, rcp_proxy_settings_check(&s));
}

TEST_CASE("On listen without a host: returns RCP_PROXY_OK", "[rcp_proxy]")
{
    radio_mux_proxy_settings_t s = defaults();

    s.adapter = RADIO_MUX_PROXY_UDP;
    TEST_ASSERT_EQUAL(RCP_PROXY_OK, rcp_proxy_settings_check(&s));
}

TEST_CASE("On framing with tcp: ignores it (kept for a later switch to udp)", "[rcp_proxy]")
{
    radio_mux_proxy_settings_t s = defaults();

    s.framing = RADIO_MUX_PROXY_RAW;
    TEST_ASSERT_EQUAL(RCP_PROXY_OK, rcp_proxy_settings_check(&s));
    s.framing = RADIO_MUX_PROXY_HDLC;
    TEST_ASSERT_EQUAL(RCP_PROXY_OK, rcp_proxy_settings_check(&s));
}

TEST_CASE("On port 80, 179 or 0: returns RCP_PROXY_BAD_PORT (above 65535 does not fit the uint16_t port)",
          "[rcp_proxy]")
{
    const uint16_t bad[] = {80, 179, 0};

    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        radio_mux_proxy_settings_t s = defaults();

        s.port = bad[i];
        TEST_ASSERT_EQUAL(RCP_PROXY_BAD_PORT, rcp_proxy_settings_check(&s));
        s = connect_to("10.0.0.1");
        s.port = bad[i];
        TEST_ASSERT_EQUAL(RCP_PROXY_BAD_PORT, rcp_proxy_settings_check(&s));
    }
}

TEST_CASE("On port 1 and port 65535: returns RCP_PROXY_OK", "[rcp_proxy]")
{
    radio_mux_proxy_settings_t s = defaults();

    s.port = 1;
    TEST_ASSERT_EQUAL(RCP_PROXY_OK, rcp_proxy_settings_check(&s));
    s.port = 65535;
    TEST_ASSERT_EQUAL(RCP_PROXY_OK, rcp_proxy_settings_check(&s));
}

TEST_CASE("On a bad port and no host for connect: returns RCP_PROXY_BAD_PORT first", "[rcp_proxy]")
{
    radio_mux_proxy_settings_t s = connect_to("");

    s.port = 80;
    TEST_ASSERT_EQUAL(RCP_PROXY_BAD_PORT, rcp_proxy_settings_check(&s));
}

TEST_CASE("On an IPv4 or IPv6 literal host: returns RCP_PROXY_OK", "[rcp_proxy]")
{
    const char *good[] = {"192.168.1.10", "0.0.0.0", "255.255.255.255", "fd00::1", "::1", "::",
                          "2001:db8:0:0:1:0:0:1", "fe80::1:2:3:4", "::ffff:192.168.1.10", "2001:DB8::A"
                         };

    for (size_t i = 0; i < sizeof(good) / sizeof(good[0]); i++) {
        radio_mux_proxy_settings_t s = connect_to(good[i]);

        TEST_ASSERT_EQUAL_MESSAGE(RCP_PROXY_OK, rcp_proxy_settings_check(&s), good[i]);
    }
}

TEST_CASE("On a DNS or .local name host: returns RCP_PROXY_OK", "[rcp_proxy]")
{
    const char *good[] = {"otbr-host.local", "pi.LOCAL", "agent.example.com", "a", "x-1.y2",
                          // 63 characters, the longest that fits
                          "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.example"
                         };

    for (size_t i = 0; i < sizeof(good) / sizeof(good[0]); i++) {
        radio_mux_proxy_settings_t s = connect_to(good[i]);

        TEST_ASSERT_EQUAL_MESSAGE(RCP_PROXY_OK, rcp_proxy_settings_check(&s), good[i]);
    }
}

TEST_CASE("On a host that is neither a literal nor a valid name: returns RCP_PROXY_BAD_HOST", "[rcp_proxy]")
{
    const char *bad[] = {"bad host", "a..b", ".a", "a.", "-a.com", "a-.com", "under_score.local", "1.2.3.256",
                         "1.2.3", "fd00:::1", "1::2::3", "fd00::1%eth0", "[fd00::1]", "host:6638", "1:2:3:4:5:6:7:8:9",
                         "12345::1", "a/b"
                        };

    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        radio_mux_proxy_settings_t s = connect_to(bad[i]);

        TEST_ASSERT_EQUAL_MESSAGE(RCP_PROXY_BAD_HOST, rcp_proxy_settings_check(&s), bad[i]);
    }
}

TEST_CASE("On a host that fills the buffer without a terminator: returns RCP_PROXY_BAD_HOST", "[rcp_proxy]")
{
    radio_mux_proxy_settings_t s = connect_to("");

    memset(s.host, 'a', sizeof(s.host));
    TEST_ASSERT_EQUAL(RCP_PROXY_BAD_HOST, rcp_proxy_settings_check(&s));
}

TEST_CASE("On a bad host in listen mode: returns RCP_PROXY_BAD_HOST (a kept host stays valid)", "[rcp_proxy]")
{
    radio_mux_proxy_settings_t s = defaults();

    strcpy(s.host, "bad host");
    TEST_ASSERT_EQUAL(RCP_PROXY_BAD_HOST, rcp_proxy_settings_check(&s));
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

/* ---- rcp_proxy_accept (TCP listen) ---- */

TEST_CASE("On a connection with no peer: returns RCP_PROXY_TAKE", "[rcp_proxy]")
{
    TEST_ASSERT_EQUAL(RCP_PROXY_TAKE, rcp_proxy_accept(false));
}

TEST_CASE("On a connection while a peer is connected: returns RCP_PROXY_REFUSE (first client wins)", "[rcp_proxy]")
{
    TEST_ASSERT_EQUAL(RCP_PROXY_REFUSE, rcp_proxy_accept(true));
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

void app_main(void)
{
    UNITY_BEGIN();
    unity_run_all_tests();
    int failures = (int)UNITY_END();
    exit(failures == 0 ? 0 : 1);
}
