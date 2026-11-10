#include <stdio.h>
#include <string.h>

#include "unity.h"
#include "zigbee_zlog_ring.h"

#define SIZE 64

static uint8_t s_buf[SIZE];
static char s_out[ZIGBEE_ZLOG_LINE_MAX + 1];

static void fresh(zigbee_zlog_ring_t *r)
{
    zigbee_zlog_ring_init(r, s_buf, SIZE);
}

static void put_str(zigbee_zlog_ring_t *r, uint8_t level, const char *s)
{
    zigbee_zlog_ring_put(r, level, (const uint8_t *)s, strlen(s));
}

TEST_CASE("one record: get returns level and text, then the ring is empty", "[zigbee]")
{
    zigbee_zlog_ring_t r;
    uint8_t level = 0;
    size_t len = 0;

    fresh(&r);
    put_str(&r, 3, "hello");
    TEST_ASSERT_TRUE(zigbee_zlog_ring_get(&r, &level, s_out, &len));
    TEST_ASSERT_EQUAL_UINT8(3, level);
    TEST_ASSERT_EQUAL_UINT(5, len);
    TEST_ASSERT_EQUAL_STRING("hello", s_out);
    TEST_ASSERT_FALSE(zigbee_zlog_ring_get(&r, &level, s_out, &len));
    TEST_ASSERT_EQUAL_UINT32(0, zigbee_zlog_ring_dropped(&r));
}

TEST_CASE("records: come out whole and in order", "[zigbee]")
{
    zigbee_zlog_ring_t r;
    uint8_t level;
    size_t len;

    fresh(&r);
    put_str(&r, 1, "a");
    put_str(&r, 2, "bc");
    put_str(&r, 5, "");
    TEST_ASSERT_TRUE(zigbee_zlog_ring_get(&r, &level, s_out, &len));
    TEST_ASSERT_EQUAL_STRING("a", s_out);
    TEST_ASSERT_TRUE(zigbee_zlog_ring_get(&r, &level, s_out, &len));
    TEST_ASSERT_EQUAL_UINT8(2, level);
    TEST_ASSERT_EQUAL_STRING("bc", s_out);
    TEST_ASSERT_TRUE(zigbee_zlog_ring_get(&r, &level, s_out, &len));
    TEST_ASSERT_EQUAL_UINT8(5, level);
    TEST_ASSERT_EQUAL_UINT(0, len);
}

TEST_CASE("full ring: the record is dropped and counted, the stored ones stay", "[zigbee]")
{
    zigbee_zlog_ring_t r;
    uint8_t level;
    size_t len;

    fresh(&r);
    put_str(&r, 3, "0123456789012345678901234567890123456789012345678901234567"); // 58 + 3 = 61 of 64
    put_str(&r, 3, "xyz");                                                           // needs 6, 3 free
    TEST_ASSERT_EQUAL_UINT32(1, zigbee_zlog_ring_dropped(&r));
    TEST_ASSERT_TRUE(zigbee_zlog_ring_get(&r, &level, s_out, &len));
    TEST_ASSERT_EQUAL_UINT(58, len);
    TEST_ASSERT_FALSE(zigbee_zlog_ring_get(&r, &level, s_out, &len));
}

TEST_CASE("record exactly filling the ring: stored", "[zigbee]")
{
    zigbee_zlog_ring_t r;
    uint8_t level;
    size_t len;
    uint8_t text[SIZE - 3];

    fresh(&r);
    memset(text, 'q', sizeof(text));
    zigbee_zlog_ring_put(&r, 4, text, sizeof(text));
    TEST_ASSERT_EQUAL_UINT32(0, zigbee_zlog_ring_dropped(&r));
    TEST_ASSERT_TRUE(zigbee_zlog_ring_get(&r, &level, s_out, &len));
    TEST_ASSERT_EQUAL_UINT(SIZE - 3, len);
}

TEST_CASE("records wrapping across the buffer end: whole and in order", "[zigbee]")
{
    zigbee_zlog_ring_t r;
    uint8_t level;
    size_t len;
    char expect[16];

    fresh(&r);
    for (int i = 0; i < 40; i++) { // 40 records of ~10 bytes: many laps of 64
        snprintf(expect, sizeof(expect), "line-%d", i);
        put_str(&r, (uint8_t)(1 + i % 5), expect);
        TEST_ASSERT_TRUE(zigbee_zlog_ring_get(&r, &level, s_out, &len));
        TEST_ASSERT_EQUAL_UINT8(1 + i % 5, level);
        TEST_ASSERT_EQUAL_STRING(expect, s_out);
    }
    TEST_ASSERT_EQUAL_UINT32(0, zigbee_zlog_ring_dropped(&r));
}

TEST_CASE("free-running indices past 2^32: the ring keeps working", "[zigbee]")
{
    zigbee_zlog_ring_t r;
    uint8_t level;
    size_t len;

    fresh(&r);
    atomic_store(&r.head, 0xFFFFFFF0u);
    atomic_store(&r.tail, 0xFFFFFFF0u);
    for (int i = 0; i < 6; i++) {
        put_str(&r, 2, "wrapping-counter");
        TEST_ASSERT_TRUE(zigbee_zlog_ring_get(&r, &level, s_out, &len));
        TEST_ASSERT_EQUAL_STRING("wrapping-counter", s_out);
    }
    TEST_ASSERT_EQUAL_UINT32(0, zigbee_zlog_ring_dropped(&r));
}

TEST_CASE("a line over 512 bytes: cut to 512", "[zigbee]")
{
    static uint8_t big[8192];
    static uint8_t text[700];
    zigbee_zlog_ring_t r;
    uint8_t level;
    size_t len;

    zigbee_zlog_ring_init(&r, big, sizeof(big));
    memset(text, 'z', sizeof(text));
    zigbee_zlog_ring_put(&r, 3, text, sizeof(text));
    TEST_ASSERT_TRUE(zigbee_zlog_ring_get(&r, &level, s_out, &len));
    TEST_ASSERT_EQUAL_UINT(ZIGBEE_ZLOG_LINE_MAX, len);
    TEST_ASSERT_EQUAL_UINT8('\0', (uint8_t)s_out[ZIGBEE_ZLOG_LINE_MAX]);
}

TEST_CASE("no buffer yet: dropped and counted, nothing to read", "[zigbee]")
{
    zigbee_zlog_ring_t r;
    uint8_t level;
    size_t len;

    zigbee_zlog_ring_init(&r, NULL, 0);
    put_str(&r, 3, "early");
    put_str(&r, 3, "early");
    TEST_ASSERT_EQUAL_UINT32(2, zigbee_zlog_ring_dropped(&r));
    TEST_ASSERT_FALSE(zigbee_zlog_ring_get(&r, &level, s_out, &len));
}
