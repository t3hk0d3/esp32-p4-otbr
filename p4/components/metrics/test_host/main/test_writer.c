#include <stdlib.h>
#include <string.h>
#include "unity.h"
#include "unity_test_runner.h"
#include "writer.h"
#include "sink.h"

static char *write_with(size_t cap, void (*body)(metrics_writer_t *))
{
    char *buf = malloc(cap);
    sink_t sink = {0};
    metrics_writer_t w;

    metrics_writer_init(&w, buf, cap, sink_flush, &sink);
    body(&w);
    TEST_ASSERT_EQUAL(ESP_OK, metrics_writer_finish(&w));
    free(buf);
    return sink.data;
}

static void long_text(metrics_writer_t *w)
{
    for (int i = 0; i < 50; i++) {
        metrics_writer_raw(w, "abcdefghij");
        metrics_writer_u64(w, 1234567890123ull);
        metrics_writer_label_value(w, "a\"b\\c\nd");
    }
}

TEST_CASE("On output larger than the buffer: flushes full buffers in order, same bytes as one large buffer", "[metrics]")
{
    char *small = write_with(7, long_text);
    char *large = write_with(65536, long_text);

    TEST_ASSERT_EQUAL_STRING(large, small);
    free(small);
    free(large);
}

static void usec_values(metrics_writer_t *w)
{
    metrics_writer_usec(w, 1500000);
    metrics_writer_raw(w, " ");
    metrics_writer_usec(w, 0);
    metrics_writer_raw(w, " ");
    metrics_writer_usec(w, 7);
    metrics_writer_raw(w, " ");
    metrics_writer_u64(w, UINT64_MAX);
}

TEST_CASE("usec and u64: microseconds as seconds with six decimals, integers in full", "[metrics]")
{
    char *out = write_with(64, usec_values);

    TEST_ASSERT_EQUAL_STRING("1.500000 0.000000 0.000007 18446744073709551615", out);
    free(out);
}

static void escapes(metrics_writer_t *w)
{
    metrics_writer_label_value(w, "back\\slash \"quote\" new\nline");
}

TEST_CASE("label_value: escapes backslash, double quote and newline", "[metrics]")
{
    char *out = write_with(64, escapes);

    TEST_ASSERT_EQUAL_STRING("back\\\\slash \\\"quote\\\" new\\nline", out);
    free(out);
}

TEST_CASE("On a flush error: stops writing and finish returns that error", "[metrics]")
{
    char buf[4];
    sink_t sink = {.fail_at = 2};
    metrics_writer_t w;

    metrics_writer_init(&w, buf, sizeof(buf), sink_flush, &sink);
    metrics_writer_raw(&w, "0123456789abcdef");
    TEST_ASSERT_EQUAL(ESP_FAIL, metrics_writer_finish(&w));
    TEST_ASSERT_EQUAL(2, sink.flushes);
    TEST_ASSERT_EQUAL_STRING("0123", sink.data);
    sink_free(&sink);
}

TEST_CASE("finish: flushes the remaining bytes once and returns ESP_OK", "[metrics]")
{
    char buf[64];
    sink_t sink = {0};
    metrics_writer_t w;

    metrics_writer_init(&w, buf, sizeof(buf), sink_flush, &sink);
    metrics_writer_raw(&w, "abc");
    TEST_ASSERT_EQUAL(ESP_OK, metrics_writer_finish(&w));
    TEST_ASSERT_EQUAL(1, sink.flushes);
    TEST_ASSERT_EQUAL_STRING("abc", sink.data);
    sink_free(&sink);
}
