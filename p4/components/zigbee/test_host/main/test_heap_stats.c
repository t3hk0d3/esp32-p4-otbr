#include <stdint.h>
#include "unity.h"
#include "zigbee_heap_stats.h"

TEST_CASE("alloc: current and peak grow by the size", "[zigbee]")
{
    zigbee_heap_stats_t st = {0};

    zigbee_heap_stats_alloc(&st, 100);
    zigbee_heap_stats_alloc(&st, 50);
    TEST_ASSERT_EQUAL(150, st.current);
    TEST_ASSERT_EQUAL(150, st.peak);
    TEST_ASSERT_EQUAL(0, st.failures);
}

TEST_CASE("free: current drops and peak stays", "[zigbee]")
{
    zigbee_heap_stats_t st = {0};

    zigbee_heap_stats_alloc(&st, 100);
    zigbee_heap_stats_alloc(&st, 50);
    zigbee_heap_stats_free(&st, 100);
    TEST_ASSERT_EQUAL(50, st.current);
    TEST_ASSERT_EQUAL(150, st.peak);
}

TEST_CASE("free of more than is counted: current stops at 0", "[zigbee]")
{
    zigbee_heap_stats_t st = {0};

    zigbee_heap_stats_alloc(&st, 10);
    zigbee_heap_stats_free(&st, 25);
    TEST_ASSERT_EQUAL(0, st.current);
    TEST_ASSERT_EQUAL(10, st.peak);
}

TEST_CASE("alloc after a free to a lower level: peak stays", "[zigbee]")
{
    zigbee_heap_stats_t st = {0};

    zigbee_heap_stats_alloc(&st, 200);
    zigbee_heap_stats_free(&st, 200);
    zigbee_heap_stats_alloc(&st, 30);
    TEST_ASSERT_EQUAL(30, st.current);
    TEST_ASSERT_EQUAL(200, st.peak);
}

TEST_CASE("realloc to a larger size: current grows by the difference and peak follows", "[zigbee]")
{
    zigbee_heap_stats_t st = {0};

    zigbee_heap_stats_alloc(&st, 50);
    zigbee_heap_stats_realloc(&st, 50, 200);
    TEST_ASSERT_EQUAL(200, st.current);
    TEST_ASSERT_EQUAL(200, st.peak);
}

TEST_CASE("realloc to a smaller size: current shrinks and peak stays", "[zigbee]")
{
    zigbee_heap_stats_t st = {0};

    zigbee_heap_stats_alloc(&st, 200);
    zigbee_heap_stats_realloc(&st, 200, 10);
    TEST_ASSERT_EQUAL(10, st.current);
    TEST_ASSERT_EQUAL(200, st.peak);
}

TEST_CASE("realloc from nothing: counts like an alloc", "[zigbee]")
{
    zigbee_heap_stats_t st = {0};

    zigbee_heap_stats_realloc(&st, 0, 64);
    TEST_ASSERT_EQUAL(64, st.current);
    TEST_ASSERT_EQUAL(64, st.peak);
}

TEST_CASE("realloc to nothing: counts like a free", "[zigbee]")
{
    zigbee_heap_stats_t st = {0};

    zigbee_heap_stats_alloc(&st, 64);
    zigbee_heap_stats_realloc(&st, 64, 0);
    TEST_ASSERT_EQUAL(0, st.current);
    TEST_ASSERT_EQUAL(64, st.peak);
}

TEST_CASE("realloc from a size larger than is counted: current stops at the new size", "[zigbee]")
{
    zigbee_heap_stats_t st = {0};

    zigbee_heap_stats_alloc(&st, 8);
    zigbee_heap_stats_realloc(&st, 100, 20);
    TEST_ASSERT_EQUAL(20, st.current);
}

TEST_CASE("failed allocation: counts a failure and leaves current and peak", "[zigbee]")
{
    zigbee_heap_stats_t st = {0};

    zigbee_heap_stats_alloc(&st, 40);
    zigbee_heap_stats_failed(&st);
    zigbee_heap_stats_failed(&st);
    TEST_ASSERT_EQUAL(2, st.failures);
    TEST_ASSERT_EQUAL(40, st.current);
    TEST_ASSERT_EQUAL(40, st.peak);
}
