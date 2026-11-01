#include "unity.h"
#include "log_limiter.h"

static log_limiter_t lim;

TEST_CASE("On a burst up to the limit: every line is forwarded", "[rcp_sync][log_limiter]")
{
    log_limiter_init(&lim, 0);
    for (int i = 0; i < LOG_LIMITER_RATE_PER_S; i++) {
        TEST_ASSERT_TRUE(log_limiter_allow(&lim, 5000));
    }
}

TEST_CASE("On a burst beyond the limit within one instant: the excess is dropped", "[rcp_sync][log_limiter]")
{
    int forwarded = 0;

    log_limiter_init(&lim, 0);
    for (int i = 0; i < 1000; i++) {
        forwarded += log_limiter_allow(&lim, 5000);
    }
    TEST_ASSERT_EQUAL(LOG_LIMITER_RATE_PER_S, forwarded);
}

TEST_CASE("On a flood of 1000 lines per second for 10 s: about 20 lines per second get through", "[rcp_sync][log_limiter]")
{
    int forwarded = 0;

    log_limiter_init(&lim, 0);
    for (uint32_t ms = 0; ms < 10000; ms++) {
        forwarded += log_limiter_allow(&lim, ms);
    }
    TEST_ASSERT_INT_WITHIN(25, 20 * 10 + 20, forwarded); // the burst plus rate * duration
}

TEST_CASE("On a pause after a flood: the bucket refills and lines flow again", "[rcp_sync][log_limiter]")
{
    log_limiter_init(&lim, 0);
    for (int i = 0; i < 100; i++) {
        log_limiter_allow(&lim, 1000);
    }
    TEST_ASSERT_FALSE(log_limiter_allow(&lim, 1000));
    TEST_ASSERT_TRUE(log_limiter_allow(&lim, 1000 + 100)); // 100 ms = two tokens at 20/s
}

TEST_CASE("On a quiet C6 (a few lines per second): nothing is ever dropped", "[rcp_sync][log_limiter]")
{
    log_limiter_init(&lim, 0);
    for (uint32_t ms = 0; ms < 60000; ms += 200) {
        TEST_ASSERT_TRUE(log_limiter_allow(&lim, ms));
    }
    TEST_ASSERT_EQUAL(0, log_limiter_take_summary(&lim, 60000));
}

TEST_CASE("On drops: the summary comes after one second with the count, then starts from zero", "[rcp_sync][log_limiter]")
{
    log_limiter_init(&lim, 0);
    for (int i = 0; i < LOG_LIMITER_RATE_PER_S + 7; i++) {
        log_limiter_allow(&lim, 2000);
    }
    TEST_ASSERT_EQUAL(0, log_limiter_take_summary(&lim, 2500));
    TEST_ASSERT_EQUAL(7, log_limiter_take_summary(&lim, 3000));
    TEST_ASSERT_EQUAL(0, log_limiter_take_summary(&lim, 4000));
}

TEST_CASE("On a flood that stopped: the summary is still available when polled later", "[rcp_sync][log_limiter]")
{
    log_limiter_init(&lim, 0);
    for (int i = 0; i < LOG_LIMITER_RATE_PER_S + 3; i++) {
        log_limiter_allow(&lim, 2000);
    }
    TEST_ASSERT_EQUAL(3, log_limiter_take_summary(&lim, 9000));
}

TEST_CASE("On a continuing flood: at most one summary per second", "[rcp_sync][log_limiter]")
{
    int summaries = 0;

    log_limiter_init(&lim, 0);
    for (uint32_t ms = 0; ms < 5000; ms++) {
        log_limiter_allow(&lim, ms);
        summaries += log_limiter_take_summary(&lim, ms) > 0;
    }
    TEST_ASSERT_LESS_OR_EQUAL(5, summaries);
    TEST_ASSERT_GREATER_OR_EQUAL(3, summaries);
}

TEST_CASE("On a millisecond clock that wraps: the limiter keeps working", "[rcp_sync][log_limiter]")
{
    log_limiter_init(&lim, 0);
    for (int i = 0; i < 100; i++) {
        log_limiter_allow(&lim, 0xfffffff0u);
    }
    TEST_ASSERT_TRUE(log_limiter_allow(&lim, 0xfffffff0u + 200u)); // wrapped to 0xb8
}
