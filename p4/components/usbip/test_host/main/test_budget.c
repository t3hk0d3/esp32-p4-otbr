#include "unity.h"
#include "unity_test_runner.h"
#include "usbip_budget.h"
#include "usbip_core.h"

static usbip_budget_t b;

TEST_CASE("endpoint with nothing in flight: grants under the hard cap, also with the shared budget used up", "[usbip]")
{
    usbip_budget_init(&b);
    // Eight IN endpoints fill the shared 32 KB with 4 KB each.
    for (uint8_t ep = 1; ep <= 8; ep++) {
        TEST_ASSERT_TRUE(usbip_budget_grant(&b, ep, true, 4096));
    }
    // Shared bytes are used up; a ninth endpoint, an OUT endpoint and EP0 each still get their reserve.
    TEST_ASSERT_TRUE(usbip_budget_grant(&b, 9, true, 4096));
    TEST_ASSERT_TRUE(usbip_budget_grant(&b, 10, false, USBIP_XFER_MAX));
    TEST_ASSERT_TRUE(usbip_budget_grant(&b, 0, false, 64));
    usbip_mem_stats_t s;
    usbip_budget_stats(&b, &s);
    TEST_ASSERT_EQUAL_UINT32(8 * 4096 + 4096 + USBIP_XFER_MAX + 64, s.inflight);
}

TEST_CASE("endpoint with nothing in flight: grants a small reserve also with the 32 transfers used up", "[usbip]")
{
    usbip_budget_init(&b);
    for (int i = 0; i < USBIP_INFLIGHT_XFERS; i++) {
        TEST_ASSERT_TRUE(usbip_budget_grant(&b, 1, true, 1));
    }
    TEST_ASSERT_FALSE(usbip_budget_grant(&b, 1, true, 1));
    TEST_ASSERT_TRUE(usbip_budget_grant(&b, 2, true, 1));
}

TEST_CASE("small first transfer: is always granted, also at the hard cap", "[usbip]")
{
    usbip_budget_init(&b);
    TEST_ASSERT_TRUE(usbip_budget_grant(&b, 1, true, USBIP_INFLIGHT_BYTES));  // IN shared full
    TEST_ASSERT_TRUE(usbip_budget_grant(&b, 2, false, USBIP_XFER_MAX));       // one large OUT
    // Charged: 32 KB + 16 KB + 14 idle x 2 KB = 76 KB; a second large one is refused.
    TEST_ASSERT_FALSE(usbip_budget_grant(&b, 3, false, USBIP_XFER_MAX));
    // Every other endpoint, EP0 included, still gets a 2 KB transfer.
    TEST_ASSERT_TRUE(usbip_budget_grant(&b, 0, true, USBIP_RESERVE_SMALL));
    for (uint8_t ep = 3; ep <= USBIP_EP_MAX; ep++) {
        TEST_ASSERT_TRUE(usbip_budget_grant(&b, ep, ep % 2 == 0, USBIP_RESERVE_SMALL));
    }
    usbip_mem_stats_t s;
    usbip_budget_stats(&b, &s);
    TEST_ASSERT_EQUAL_UINT32(USBIP_INFLIGHT_BYTES + USBIP_XFER_MAX + 14 * USBIP_RESERVE_SMALL, s.inflight);
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(USBIP_INFLIGHT_HARD, s.inflight);
}

TEST_CASE("large first transfer: is refused over the hard cap and granted under it", "[usbip]")
{
    usbip_budget_init(&b);
    TEST_ASSERT_TRUE(usbip_budget_grant(&b, 1, true, USBIP_INFLIGHT_BYTES));
    TEST_ASSERT_TRUE(usbip_budget_grant(&b, 2, false, USBIP_XFER_MAX));  // charged 76 KB
    TEST_ASSERT_FALSE(usbip_budget_grant(&b, 3, false, USBIP_XFER_MAX));  // would be 90 KB
    TEST_ASSERT_FALSE(usbip_budget_grant(&b, 0, false, 6 * 1024 + 1));  // EP0: one byte over 80 KB
    TEST_ASSERT_TRUE(usbip_budget_grant(&b, 0, false, 6 * 1024));       // exactly 80 KB
    usbip_budget_release(&b, 0, 6 * 1024);
    usbip_budget_release(&b, 2, USBIP_XFER_MAX);
    // The bytes are free again: the waiting large transfer now fits.
    TEST_ASSERT_TRUE(usbip_budget_grant(&b, 3, false, USBIP_XFER_MAX));
}

TEST_CASE("large first OUT transfer: always fits while IN uses only its shared 32 KB", "[usbip]")
{
    usbip_budget_init(&b);
    // Eight IN endpoints hold the shared bytes, EP0 and six more endpoints hold small reserves.
    for (uint8_t ep = 1; ep <= 8; ep++) {
        TEST_ASSERT_TRUE(usbip_budget_grant(&b, ep, true, 4096));
    }
    TEST_ASSERT_TRUE(usbip_budget_grant(&b, 0, true, USBIP_RESERVE_SMALL));
    for (uint8_t ep = 9; ep < USBIP_EP_MAX; ep++) {
        TEST_ASSERT_TRUE(usbip_budget_grant(&b, ep, true, USBIP_RESERVE_SMALL));
    }
    TEST_ASSERT_TRUE(usbip_budget_grant(&b, USBIP_EP_MAX, false, USBIP_XFER_MAX));
}

TEST_CASE("large first IN transfer: takes the shared bytes when they fit, the hard cap when not", "[usbip]")
{
    usbip_budget_init(&b);
    TEST_ASSERT_TRUE(usbip_budget_grant(&b, 1, true, USBIP_XFER_MAX));  // shared: 16 KB
    TEST_ASSERT_TRUE(usbip_budget_grant(&b, 2, true, USBIP_XFER_MAX));  // shared: 32 KB
    TEST_ASSERT_TRUE(usbip_budget_grant(&b, 3, true, USBIP_XFER_MAX));  // large: charged 32 + 16 + 26 = 74 KB
    TEST_ASSERT_FALSE(usbip_budget_grant(&b, 4, true, USBIP_XFER_MAX));  // large: 88 KB, refused
    TEST_ASSERT_FALSE(usbip_budget_grant(&b, 1, true, 1));  // shared still full
}

TEST_CASE("any sequence of grants and releases: in-flight bytes never exceed 80 KB", "[usbip]")
{
    static const size_t sizes[] = {8, 64, 512, USBIP_RESERVE_SMALL, USBIP_RESERVE_SMALL + 1, 4096, 8192,
                                   USBIP_XFER_MAX
                                  };
    size_t held[USBIP_EP_MAX + 1][USBIP_INFLIGHT_XFERS + 1] = {0};
    size_t count[USBIP_EP_MAX + 1] = {0};
    uint32_t seed = 12345;
    size_t peak = 0;

    usbip_budget_init(&b);
    for (int step = 0; step < 200000; step++) {
        seed = seed * 1103515245u + 12345u;
        uint8_t ep = (uint8_t)((seed >> 8) % (USBIP_EP_MAX + 1));
        bool in = ep != 0 && ((seed >> 4) & 1);
        size_t n = sizes[(seed >> 16) % (sizeof(sizes) / sizeof(sizes[0]))];

        if (((seed >> 24) % 3) == 0 && count[ep] > 0) {
            usbip_budget_release(&b, ep, held[ep][--count[ep]]);
        } else if (count[ep] <= USBIP_INFLIGHT_XFERS && usbip_budget_grant(&b, ep, in, n)) {
            held[ep][count[ep]++] = n;
        }
        usbip_mem_stats_t s;
        usbip_budget_stats(&b, &s);
        TEST_ASSERT_LESS_OR_EQUAL_UINT32(USBIP_INFLIGHT_HARD, s.inflight);
        peak = s.inflight > peak ? s.inflight : peak;
    }
    TEST_ASSERT_GREATER_THAN_UINT32(USBIP_INFLIGHT_BYTES, peak);  // the sequence did go past the shared bytes
}

TEST_CASE("IN endpoint with transfers in flight: grants while bytes and count fit", "[usbip]")
{
    usbip_budget_init(&b);
    TEST_ASSERT_TRUE(usbip_budget_grant(&b, 1, true, 16384));
    TEST_ASSERT_TRUE(usbip_budget_grant(&b, 1, true, 16384));  // exactly 32768
    usbip_mem_stats_t s;
    usbip_budget_stats(&b, &s);
    TEST_ASSERT_EQUAL_UINT32(USBIP_INFLIGHT_BYTES, s.inflight);
}

TEST_CASE("IN endpoint with transfers in flight: refuses beyond the shared bytes", "[usbip]")
{
    usbip_budget_init(&b);
    TEST_ASSERT_TRUE(usbip_budget_grant(&b, 1, true, 16384));
    TEST_ASSERT_TRUE(usbip_budget_grant(&b, 1, true, 16384));
    TEST_ASSERT_FALSE(usbip_budget_grant(&b, 1, true, 1));
    usbip_mem_stats_t s;
    usbip_budget_stats(&b, &s);
    TEST_ASSERT_EQUAL_UINT32(USBIP_INFLIGHT_BYTES, s.inflight);
}

TEST_CASE("IN endpoint with transfers in flight: refuses beyond 32 transfers", "[usbip]")
{
    usbip_budget_init(&b);
    for (int i = 0; i < USBIP_INFLIGHT_XFERS; i++) {
        TEST_ASSERT_TRUE(usbip_budget_grant(&b, 3, true, 8));
    }
    TEST_ASSERT_FALSE(usbip_budget_grant(&b, 3, true, 8));
}

TEST_CASE("OUT endpoint with a transfer in flight: refuses", "[usbip]")
{
    usbip_budget_init(&b);
    TEST_ASSERT_TRUE(usbip_budget_grant(&b, 2, false, 64));
    TEST_ASSERT_FALSE(usbip_budget_grant(&b, 2, false, 64));
    // Another endpoint is unaffected.
    TEST_ASSERT_TRUE(usbip_budget_grant(&b, 3, false, 64));
}

TEST_CASE("EP0 with a transfer in flight: refuses a second", "[usbip]")
{
    usbip_budget_init(&b);
    TEST_ASSERT_TRUE(usbip_budget_grant(&b, 0, true, 64));  // is_in does not make EP0 multi-transfer
    TEST_ASSERT_FALSE(usbip_budget_grant(&b, 0, true, 64));
    usbip_budget_release(&b, 0, 64);
    TEST_ASSERT_TRUE(usbip_budget_grant(&b, 0, false, 64));
}

TEST_CASE("endpoint index out of range: grant refuses, release is ignored", "[usbip]")
{
    usbip_budget_init(&b);
    TEST_ASSERT_FALSE(usbip_budget_grant(&b, USBIP_EP_MAX + 1, true, 8));
    TEST_ASSERT_FALSE(usbip_budget_grant(&b, 255, false, 8));
    TEST_ASSERT_TRUE(usbip_budget_grant(&b, USBIP_EP_MAX, true, 8));
    usbip_budget_release(&b, USBIP_EP_MAX + 1, 8);
    usbip_mem_stats_t s;
    usbip_budget_stats(&b, &s);
    TEST_ASSERT_EQUAL_UINT32(8, s.inflight);
}

TEST_CASE("release: frees the bytes and the endpoint's count, peaks are kept", "[usbip]")
{
    usbip_budget_init(&b);
    TEST_ASSERT_TRUE(usbip_budget_grant(&b, 2, false, 1000));
    TEST_ASSERT_TRUE(usbip_budget_grant(&b, 1, true, 500));
    usbip_budget_release(&b, 2, 1000);
    usbip_mem_stats_t s;
    usbip_budget_stats(&b, &s);
    TEST_ASSERT_EQUAL_UINT32(500, s.inflight);
    TEST_ASSERT_EQUAL_UINT32(1500, s.inflight_peak);
    // The OUT endpoint is free again.
    TEST_ASSERT_TRUE(usbip_budget_grant(&b, 2, false, 10));
}

TEST_CASE("release: more bytes than in flight clamps to zero", "[usbip]")
{
    usbip_budget_init(&b);
    TEST_ASSERT_TRUE(usbip_budget_grant(&b, 1, true, 100));
    usbip_budget_release(&b, 1, 5000);
    usbip_mem_stats_t s;
    usbip_budget_stats(&b, &s);
    TEST_ASSERT_EQUAL_UINT32(0, s.inflight);
    // A release with nothing in flight stays at zero too, and the count does not go negative.
    usbip_budget_release(&b, 1, 10);
    TEST_ASSERT_TRUE(usbip_budget_grant(&b, 1, true, 100));
    TEST_ASSERT_TRUE(usbip_budget_grant(&b, 1, true, 100));
    usbip_budget_stats(&b, &s);
    TEST_ASSERT_EQUAL_UINT32(200, s.inflight);
}

TEST_CASE("wait_add: accepts up to 1 MiB of waiting bytes, refuses beyond", "[usbip]")
{
    usbip_budget_init(&b);
    TEST_ASSERT_TRUE(usbip_budget_wait_add(&b, USBIP_WAIT_BYTES - 10));
    TEST_ASSERT_TRUE(usbip_budget_wait_add(&b, 10));  // exactly the cap
    TEST_ASSERT_FALSE(usbip_budget_wait_add(&b, 1));
    usbip_mem_stats_t s;
    usbip_budget_stats(&b, &s);
    TEST_ASSERT_EQUAL_UINT32(USBIP_WAIT_BYTES, s.waiting);
}

TEST_CASE("wait_add: a refused add changes nothing, wait_sub frees and clamps", "[usbip]")
{
    usbip_budget_init(&b);
    TEST_ASSERT_TRUE(usbip_budget_wait_add(&b, 1000));
    TEST_ASSERT_FALSE(usbip_budget_wait_add(&b, USBIP_WAIT_BYTES));
    usbip_mem_stats_t s;
    usbip_budget_stats(&b, &s);
    TEST_ASSERT_EQUAL_UINT32(1000, s.waiting);
    usbip_budget_wait_sub(&b, 400);
    usbip_budget_stats(&b, &s);
    TEST_ASSERT_EQUAL_UINT32(600, s.waiting);
    TEST_ASSERT_EQUAL_UINT32(1000, s.waiting_peak);
    usbip_budget_wait_sub(&b, 99999);
    usbip_budget_stats(&b, &s);
    TEST_ASSERT_EQUAL_UINT32(0, s.waiting);
}

TEST_CASE("stats: reports in-flight and waiting bytes and their peaks", "[usbip]")
{
    usbip_budget_init(&b);
    usbip_mem_stats_t s;
    usbip_budget_stats(&b, &s);
    TEST_ASSERT_EQUAL_UINT32(0, s.inflight + s.inflight_peak + s.waiting + s.waiting_peak);
    TEST_ASSERT_TRUE(usbip_budget_grant(&b, 1, true, 300));
    TEST_ASSERT_TRUE(usbip_budget_wait_add(&b, 700));
    usbip_budget_release(&b, 1, 300);
    usbip_budget_wait_sub(&b, 700);
    usbip_budget_stats(&b, &s);
    TEST_ASSERT_EQUAL_UINT32(0, s.inflight);
    TEST_ASSERT_EQUAL_UINT32(300, s.inflight_peak);
    TEST_ASSERT_EQUAL_UINT32(0, s.waiting);
    TEST_ASSERT_EQUAL_UINT32(700, s.waiting_peak);
}
