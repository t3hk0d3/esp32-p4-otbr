#include <string.h>
#include "unity.h"
#include "unity_test_runner.h"
#include "lock_stats.h"

#define S 1000000ULL

TEST_CASE("On acquire followed by release: increments acquisitions", "[diag]")
{
    diag_lock_stats_t s = {0};
    diag_lock_view_t v;

    diag_lock_on_acquire(&s, 10 * S, 0, true);
    diag_lock_on_release(&s, 10 * S + 500, "ot");
    diag_lock_on_acquire(&s, 11 * S, 0, true);
    diag_lock_on_release(&s, 11 * S + 100, "ot");
    diag_lock_view(&s, 12 * S, &v);
    TEST_ASSERT_EQUAL(2, v.since_boot.acquisitions);
    TEST_ASSERT_EQUAL(2, v.last_60s.acquisitions);
}

TEST_CASE("On acquire followed by release: updates max_hold_us when the hold time exceeds the previous maximum", "[diag]")
{
    diag_lock_stats_t s = {0};
    diag_lock_view_t v;

    diag_lock_on_acquire(&s, 10 * S, 0, true);
    diag_lock_on_release(&s, 10 * S + 500, "slow");
    diag_lock_on_acquire(&s, 11 * S, 0, true);
    diag_lock_on_release(&s, 11 * S + 100, "fast"); // shorter: the maximum stays
    diag_lock_view(&s, 12 * S, &v);
    TEST_ASSERT_EQUAL(500, v.since_boot.max_hold_us);
    TEST_ASSERT_EQUAL_STRING("slow", v.since_boot.hold_task);
    TEST_ASSERT_EQUAL(500, v.last_60s.max_hold_us);
    TEST_ASSERT_EQUAL_STRING("slow", v.last_60s.hold_task);
}

TEST_CASE("On acquire that waits: updates max_wait_us", "[diag]")
{
    diag_lock_stats_t s = {0};
    diag_lock_view_t v;

    diag_lock_on_acquire(&s, 10 * S, 300, true);
    diag_lock_on_release(&s, 10 * S + 10, "a");
    diag_lock_on_acquire(&s, 11 * S, 40, true);
    diag_lock_on_release(&s, 11 * S + 10, "a");
    diag_lock_view(&s, 12 * S, &v);
    TEST_ASSERT_EQUAL(300, v.since_boot.max_wait_us);
    TEST_ASSERT_EQUAL(300, v.last_60s.max_wait_us);
}

TEST_CASE("On acquire that times out: records the wait but not an acquisition", "[diag]")
{
    diag_lock_stats_t s = {0};
    diag_lock_view_t v;

    diag_lock_on_acquire(&s, 10 * S, 900, false);
    diag_lock_view(&s, 10 * S, &v);
    TEST_ASSERT_EQUAL(900, v.since_boot.max_wait_us);
    TEST_ASSERT_EQUAL(0, v.since_boot.acquisitions);
    TEST_ASSERT_EQUAL(0, s.depth);
}

TEST_CASE("On nested acquire by the holder: only the outermost pair counts and times the hold", "[diag]")
{
    diag_lock_stats_t s = {0};
    diag_lock_view_t v;

    diag_lock_on_acquire(&s, 10 * S, 0, true);
    diag_lock_on_acquire(&s, 10 * S + 100, 0, true); // recursive
    diag_lock_on_release(&s, 10 * S + 200, "ot");    // inner release: hold continues
    diag_lock_view(&s, 10 * S + 200, &v);
    TEST_ASSERT_EQUAL(0, v.since_boot.max_hold_us);
    diag_lock_on_release(&s, 10 * S + 900, "ot");
    diag_lock_view(&s, 11 * S, &v);
    TEST_ASSERT_EQUAL(1, v.since_boot.acquisitions);
    TEST_ASSERT_EQUAL(900, v.since_boot.max_hold_us);
}

TEST_CASE("On acquires, including timeouts: adds every wait to wait_us_total", "[diag]")
{
    diag_lock_stats_t s = {0};
    diag_lock_view_t v;

    diag_lock_on_acquire(&s, 10 * S, 30, true);
    diag_lock_on_release(&s, 10 * S + 100, "ot");
    diag_lock_on_acquire(&s, 11 * S, 70, false);
    diag_lock_view(&s, 12 * S, &v);
    TEST_ASSERT_EQUAL_UINT64(100, v.wait_us_total);
}

TEST_CASE("On outermost releases: adds each hold to hold_us_total; nested releases add nothing", "[diag]")
{
    diag_lock_stats_t s = {0};
    diag_lock_view_t v;

    diag_lock_on_acquire(&s, 10 * S, 0, true);
    diag_lock_on_acquire(&s, 10 * S + 100, 0, true);
    diag_lock_on_release(&s, 10 * S + 200, "ot");
    diag_lock_on_release(&s, 10 * S + 900, "ot");
    diag_lock_on_acquire(&s, 11 * S, 0, true);
    diag_lock_on_release(&s, 11 * S + 100, "ot");
    diag_lock_view(&s, 12 * S, &v);
    TEST_ASSERT_EQUAL_UINT64(1000, v.hold_us_total);
}

TEST_CASE("On release of a free lock: ignored", "[diag]")
{
    diag_lock_stats_t s = {0};
    diag_lock_view_t v;

    diag_lock_on_release(&s, 10 * S, "stray");
    diag_lock_on_acquire(&s, 11 * S, 0, true);
    diag_lock_on_release(&s, 11 * S + 50, "ot");
    diag_lock_view(&s, 12 * S, &v);
    TEST_ASSERT_EQUAL(50, v.since_boot.max_hold_us);
    TEST_ASSERT_EQUAL(0, s.depth);
}

TEST_CASE("On figures older than 60 s: the last-60-s view forgets them, since-boot keeps them", "[diag]")
{
    diag_lock_stats_t s = {0};
    diag_lock_view_t v;

    diag_lock_on_acquire(&s, 10 * S, 700, true);
    diag_lock_on_release(&s, 10 * S + 800, "old");
    diag_lock_view(&s, 69 * S, &v); // second 10 is the 60th second back: still inside
    TEST_ASSERT_EQUAL(800, v.last_60s.max_hold_us);
    diag_lock_view(&s, 70 * S, &v);
    TEST_ASSERT_EQUAL(0, v.last_60s.max_hold_us);
    TEST_ASSERT_EQUAL(0, v.last_60s.max_wait_us);
    TEST_ASSERT_EQUAL(0, v.last_60s.acquisitions);
    TEST_ASSERT_EQUAL(800, v.since_boot.max_hold_us);
    TEST_ASSERT_EQUAL(700, v.since_boot.max_wait_us);
}

TEST_CASE("On a long task name: it is truncated and terminated", "[diag]")
{
    diag_lock_stats_t s = {0};
    diag_lock_view_t v;

    diag_lock_on_acquire(&s, S, 0, true);
    diag_lock_on_release(&s, S + 5, "a_very_long_task_name_indeed");
    diag_lock_view(&s, 2 * S, &v);
    TEST_ASSERT_EQUAL(DIAG_LOCK_NAME_MAX - 1, strlen(v.since_boot.hold_task));
}
