#include <stdio.h>
#include <string.h>
#include "unity.h"
#include "logging_ring.h"

static uint8_t s_buf[1024];
static char s_text[LOGGING_LINE_MAX];

static logging_line_t meta(esp_log_level_t level, const char *tag, uint32_t uptime_ms)
{
    logging_line_t m = {.uptime_ms = uptime_ms, .level = level};

    strlcpy(m.tag, tag, sizeof(m.tag));
    return m;
}

static bool push(logging_ring_t *ring, uint32_t uptime_ms, const char *text)
{
    logging_ring_slot_t slot;
    logging_line_t m = meta(ESP_LOG_INFO, "ziggurat", uptime_ms);
    size_t len = strlen(text);

    if (!logging_ring_reserve(ring, len, &slot)) {
        return false;
    }
    memcpy(slot.text, text, len);
    logging_ring_commit(ring, &slot, &m, len);
    return true;
}

static void expect_pop(logging_ring_t *ring, uint32_t uptime_ms, const char *text)
{
    logging_line_t out;

    TEST_ASSERT_TRUE(logging_ring_pop(ring, &out, s_text, sizeof(s_text)));
    TEST_ASSERT_EQUAL_UINT32(uptime_ms, out.uptime_ms);
    TEST_ASSERT_EQUAL(strlen(text), out.len);
    TEST_ASSERT_EQUAL_PTR(s_text, out.text);
    TEST_ASSERT_EQUAL_MEMORY(text, out.text, out.len);
}

/* ---- reserve / commit / pop ---- */

TEST_CASE("reserve+commit of one record: pop returns it with level, tag, uptime, text intact", "[logging]")
{
    logging_ring_t ring;
    logging_ring_slot_t slot;
    logging_line_t m = meta(ESP_LOG_WARN, "radio_mux", 813120);
    logging_line_t out;

    logging_ring_init(&ring, s_buf, sizeof(s_buf));
    TEST_ASSERT_TRUE(logging_ring_reserve(&ring, 7, &slot));
    TEST_ASSERT_GREATER_OR_EQUAL(7, slot.cap);
    memcpy(slot.text, "chan up", 7);
    logging_ring_commit(&ring, &slot, &m, 7);

    TEST_ASSERT_TRUE(logging_ring_pop(&ring, &out, s_text, sizeof(s_text)));
    TEST_ASSERT_EQUAL(ESP_LOG_WARN, out.level);
    TEST_ASSERT_EQUAL_STRING("radio_mux", out.tag);
    TEST_ASSERT_EQUAL_UINT32(813120, out.uptime_ms);
    TEST_ASSERT_EQUAL(7, out.len);
    TEST_ASSERT_EQUAL_MEMORY("chan up", out.text, 7);
    TEST_ASSERT_FALSE(logging_ring_pop(&ring, &out, s_text, sizeof(s_text)));
    TEST_ASSERT_EQUAL_UINT32(0, logging_ring_dropped(&ring));
}

TEST_CASE("wrap-around at the buffer end: records stay whole and in order", "[logging]")
{
    logging_ring_t ring;
    char text[160];
    uint32_t pushed = 0;
    uint32_t popped = 0;

    // Unaligned start and odd size: records still line up. Three records in flight, many laps of the buffer, with
    // lengths that vary so the end gap takes every size (including too small for a header).
    logging_ring_init(&ring, s_buf + 1, 901);
    for (int round = 0; round < 200; round++) {
        while (pushed - popped < 3) {
            size_t len = 1 + (pushed * 37) % (sizeof(text) - 1);

            memset(text, 'a' + pushed % 26, len);
            text[len] = '\0';
            TEST_ASSERT_TRUE(push(&ring, pushed, text));
            pushed++;
        }
        size_t len = 1 + (popped * 37) % (sizeof(text) - 1);

        memset(text, 'a' + popped % 26, len);
        text[len] = '\0';
        expect_pop(&ring, popped, text);
        popped++;
    }
    TEST_ASSERT_EQUAL_UINT32(0, logging_ring_dropped(&ring));
}

TEST_CASE("reserve with not enough free space: returns false, dropped counter + 1, nothing written", "[logging]")
{
    logging_ring_t ring;
    logging_ring_slot_t slot;
    char text[100];
    int n = 0;

    memset(text, 'x', sizeof(text) - 1);
    text[sizeof(text) - 1] = '\0';
    logging_ring_init(&ring, s_buf, 512);
    while (push(&ring, n, text)) {
        n++;
    }
    TEST_ASSERT_GREATER_THAN(1, n);
    TEST_ASSERT_EQUAL_UINT32(1, logging_ring_dropped(&ring));
    TEST_ASSERT_FALSE(logging_ring_reserve(&ring, 99, &slot));
    TEST_ASSERT_EQUAL_UINT32(2, logging_ring_dropped(&ring));

    for (int i = 0; i < n; i++) {
        expect_pop(&ring, i, text);
    }
    logging_line_t out;

    TEST_ASSERT_FALSE(logging_ring_pop(&ring, &out, s_text, sizeof(s_text)));
}

TEST_CASE("reserved but uncommitted record ahead of a committed one: pop waits until it is committed", "[logging]")
{
    logging_ring_t ring;
    logging_ring_slot_t a;
    logging_ring_slot_t b;
    logging_line_t ma = meta(ESP_LOG_INFO, "first", 1);
    logging_line_t mb = meta(ESP_LOG_INFO, "second", 2);
    logging_line_t out;

    logging_ring_init(&ring, s_buf, sizeof(s_buf));
    TEST_ASSERT_TRUE(logging_ring_reserve(&ring, 16, &a));
    TEST_ASSERT_TRUE(logging_ring_reserve(&ring, 16, &b));
    memcpy(b.text, "bbb", 3);
    logging_ring_commit(&ring, &b, &mb, 3);
    TEST_ASSERT_FALSE(logging_ring_pop(&ring, &out, s_text, sizeof(s_text)));

    memcpy(a.text, "aaa", 3);
    logging_ring_commit(&ring, &a, &ma, 3);
    TEST_ASSERT_TRUE(logging_ring_pop(&ring, &out, s_text, sizeof(s_text)));
    TEST_ASSERT_EQUAL_STRING("first", out.tag);
    TEST_ASSERT_EQUAL_MEMORY("aaa", out.text, 3);
    TEST_ASSERT_TRUE(logging_ring_pop(&ring, &out, s_text, sizeof(s_text)));
    TEST_ASSERT_EQUAL_STRING("second", out.tag);
    TEST_ASSERT_EQUAL_MEMORY("bbb", out.text, 3);
}

TEST_CASE("commit with fewer bytes than reserved: pop returns only the committed length", "[logging]")
{
    logging_ring_t ring;
    logging_ring_slot_t slot;
    logging_line_t m = meta(ESP_LOG_DEBUG, "wifi", 5);
    logging_line_t out;

    logging_ring_init(&ring, s_buf, sizeof(s_buf));
    TEST_ASSERT_TRUE(logging_ring_reserve(&ring, LOGGING_LINE_MAX, &slot));
    memset(slot.text, 'z', slot.cap);
    memcpy(slot.text, "hello", 5);
    logging_ring_commit(&ring, &slot, &m, 5);
    TEST_ASSERT_TRUE(logging_ring_pop(&ring, &out, s_text, sizeof(s_text)));
    TEST_ASSERT_EQUAL(5, out.len);
    TEST_ASSERT_EQUAL_MEMORY("hello", out.text, 5);
}
