#include <stdio.h>
#include <string.h>
#include "unity.h"
#include "logging.h"
#include "logging_text.h"

static void expect_target(const char *in, const char *host, uint16_t port)
{
    char h[64];
    uint16_t p = 0;

    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, logging_target_parse(in, h, sizeof(h), &p), in);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(host, h, in);
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(port, p, in);
}

/* ---- logging_target_parse ---- */

TEST_CASE("valid targets: host and port, 514 when omitted", "[logging]")
{
    expect_target("192.168.1.10", "192.168.1.10", 514);
    expect_target("192.168.1.10:1514", "192.168.1.10", 1514);
    expect_target("[fd00::1]", "fd00::1", 514);
    expect_target("[fd00::1]:1514", "fd00::1", 1514);
    expect_target("logs.lan:514", "logs.lan", 514);
    expect_target("logs.lan:65535", "logs.lan", 65535);
}

TEST_CASE("invalid targets: ESP_ERR_INVALID_ARG, outputs untouched", "[logging]")
{
    static const char *const bad[] = {"", ":514", "host:0", "host:65536", "host:x", "host:", "[fd00::1", "fd00::1:514",
                                      "[]", "[fd00::1]x", "[fd00::1]:", "ho st",
                                      "host:5 14"
                                     };

    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        char h[64] = "keep";
        uint16_t p = 7;

        TEST_ASSERT_EQUAL_MESSAGE(ESP_ERR_INVALID_ARG, logging_target_parse(bad[i], h, sizeof(h), &p), bad[i]);
        TEST_ASSERT_EQUAL_STRING("keep", h);
        TEST_ASSERT_EQUAL_UINT16(7, p);
    }
    char h[4];
    uint16_t p;

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, logging_target_parse(NULL, h, sizeof(h), &p));
}

TEST_CASE("host longer than the buffer: ESP_ERR_INVALID_ARG", "[logging]")
{
    char h[8];
    uint16_t p;

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, logging_target_parse("longhostname.lan", h, sizeof(h), &p));
    TEST_ASSERT_EQUAL(ESP_OK, logging_target_parse("1234567", h, sizeof(h), &p));
}

/* ---- logging_text_body ---- */

static size_t body_of(const char *in, const char **body)
{
    return logging_text_body(in, strlen(in), body);
}

TEST_CASE("esp_log line with colour and newline: body is the message", "[logging]")
{
    const char *body;
    size_t n = body_of("\033[0;32mI (1234) radio_mux: chan up\033[0m\n", &body);

    TEST_ASSERT_EQUAL_UINT(7, n);
    TEST_ASSERT_EQUAL_STRING_LEN("chan up", body, n);
}

TEST_CASE("esp_log line without colour, with CRLF: body is the message", "[logging]")
{
    const char *body;
    size_t n = body_of("W (5) zigbee: join failed\r\n", &body);

    TEST_ASSERT_EQUAL_UINT(11, n);
    TEST_ASSERT_EQUAL_STRING_LEN("join failed", body, n);
}

TEST_CASE("fragment without prefix or newline: body unchanged", "[logging]")
{
    const char *body;
    const char *in = "partial: 12 (x) text";
    size_t n = body_of(in, &body);

    TEST_ASSERT_EQUAL_UINT(strlen(in), n);
    TEST_ASSERT_EQUAL_STRING_LEN(in, body, n);
}

TEST_CASE("empty input: empty body", "[logging]")
{
    const char *body;

    TEST_ASSERT_EQUAL_UINT(0, logging_text_body("", 0, &body));
}

TEST_CASE("line longer than LOGGING_LINE_MAX: length clamped to one past the cap", "[logging]")
{
    char in[LOGGING_LINE_MAX + 100];
    const char *body;

    memset(in, 'a', sizeof(in));
    TEST_ASSERT_EQUAL_UINT(LOGGING_LINE_MAX + 1, logging_text_body(in, sizeof(in), &body));
    TEST_ASSERT_EQUAL_PTR(in, body);
}

/* ---- logging_text_truncate ---- */

TEST_CASE("body at the cap: kept as is", "[logging]")
{
    char buf[LOGGING_LINE_MAX + 1];

    memset(buf, 'a', LOGGING_LINE_MAX);
    TEST_ASSERT_EQUAL_UINT(LOGGING_LINE_MAX, logging_text_truncate(buf, LOGGING_LINE_MAX, LOGGING_LINE_MAX));
    TEST_ASSERT_EQUAL_CHAR('a', buf[LOGGING_LINE_MAX - 1]);
}

TEST_CASE("body over the cap: LOGGING_LINE_MAX bytes ending in the cut marker", "[logging]")
{
    char buf[LOGGING_LINE_MAX + 1];
    static const char marker[] = "\xe2\x80\xa6[cut]";

    memset(buf, 'a', sizeof(buf));
    TEST_ASSERT_EQUAL_UINT(LOGGING_LINE_MAX, logging_text_truncate(buf, LOGGING_LINE_MAX + 1, LOGGING_LINE_MAX));
    TEST_ASSERT_EQUAL_MEMORY(marker, buf + LOGGING_LINE_MAX - strlen(marker), strlen(marker));
    TEST_ASSERT_EQUAL_CHAR('a', buf[LOGGING_LINE_MAX - strlen(marker) - 1]);
}

TEST_CASE("cut inside a multi-byte character: the character is dropped whole", "[logging]")
{
    char buf[LOGGING_LINE_MAX + 1];
    size_t keep = LOGGING_LINE_MAX - strlen("\xe2\x80\xa6[cut]");

    memset(buf, 'a', sizeof(buf));
    buf[keep - 1] = (char) 0xc3; /* lead byte straddling the cut */
    buf[keep] = (char) 0xa9;
    size_t n = logging_text_truncate(buf, LOGGING_LINE_MAX + 1, LOGGING_LINE_MAX);

    TEST_ASSERT_EQUAL_UINT(LOGGING_LINE_MAX - 1, n);
    TEST_ASSERT_EQUAL_CHAR('a', buf[keep - 2]);
    TEST_ASSERT_EQUAL_MEMORY("\xe2\x80\xa6[cut]", buf + keep - 1, 8);
}

/* ---- logging_syslog_format ---- */

static int fmt(char *out, size_t cap, esp_log_level_t level, const char *tag, const struct timeval *wall)
{
    return logging_syslog_format(out, cap, level, "esp-otbr-d2cafc", tag, 813120, wall, "request timeout",
                                 strlen("request timeout"));
}

TEST_CASE("warning without clock: exact RFC 5424 line", "[logging]")
{
    char out[256];
    static const char want[] = "<132>1 - esp-otbr-d2cafc zigbee - - - 813120 request timeout";

    TEST_ASSERT_EQUAL_INT((int) strlen(want), fmt(out, sizeof(out), ESP_LOG_WARN, "zigbee", NULL));
    TEST_ASSERT_EQUAL_STRING(want, out);
}

TEST_CASE("valid clock: TIMESTAMP is UTC with milliseconds", "[logging]")
{
    char out[256];
    struct timeval tv = {.tv_sec = 1794312000, .tv_usec = 123456}; /* 2026-11-10 12:00:00 UTC, plus 123 ms */

    TEST_ASSERT_GREATER_THAN(0, fmt(out, sizeof(out), ESP_LOG_WARN, "zigbee", &tv));
    TEST_ASSERT_EQUAL_STRING_LEN("<132>1 2026-11-10T12:00:00.123Z esp-otbr-d2cafc zigbee", out, 53);
}

TEST_CASE("each level maps to its severity in PRI", "[logging]")
{
    static const struct {
        esp_log_level_t level;
        const char *pri;
    } map[] = {{ESP_LOG_ERROR, "<131>"}, {ESP_LOG_WARN, "<132>"}, {ESP_LOG_INFO, "<134>"},
        {ESP_LOG_DEBUG, "<135>"}, {ESP_LOG_VERBOSE, "<135>"}
    };

    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        char out[256];

        TEST_ASSERT_GREATER_THAN(0, fmt(out, sizeof(out), map[i].level, "t", NULL));
        TEST_ASSERT_EQUAL_STRING_LEN(map[i].pri, out, 5);
    }
}

TEST_CASE("tag with a space: APP-NAME sanitized", "[logging]")
{
    char out[256];

    TEST_ASSERT_GREATER_THAN(0, fmt(out, sizeof(out), ESP_LOG_INFO, "my tag", NULL));
    TEST_ASSERT_EQUAL_STRING("<134>1 - esp-otbr-d2cafc my_tag - - - 813120 request timeout", out);
}

TEST_CASE("tag over 48 chars: APP-NAME cut at 48", "[logging]")
{
    char out[256];
    char tag[80];
    char want[256];

    memset(tag, 'x', sizeof(tag));
    tag[79] = '\0';
    TEST_ASSERT_GREATER_THAN(0, fmt(out, sizeof(out), ESP_LOG_INFO, tag, NULL));
    snprintf(want, sizeof(want), "<134>1 - esp-otbr-d2cafc %.48s - - - 813120 request timeout", tag);
    TEST_ASSERT_EQUAL_STRING(want, out);
}

TEST_CASE("empty tag: APP-NAME is NILVALUE", "[logging]")
{
    char out[256];

    TEST_ASSERT_GREATER_THAN(0, fmt(out, sizeof(out), ESP_LOG_INFO, "", NULL));
    TEST_ASSERT_EQUAL_STRING("<134>1 - esp-otbr-d2cafc - - - - 813120 request timeout", out);
}

TEST_CASE("output shorter than needed: -1 and out NUL-terminated", "[logging]")
{
    char out[20];

    memset(out, 'z', sizeof(out));
    TEST_ASSERT_EQUAL_INT(-1, fmt(out, sizeof(out), ESP_LOG_WARN, "zigbee", NULL));
    TEST_ASSERT_EQUAL_CHAR('\0', out[sizeof(out) - 1]);
    TEST_ASSERT_LESS_THAN(sizeof(out), strlen(out));
}

TEST_CASE("output of exactly the needed size: fits, one byte less does not", "[logging]")
{
    char out[256];
    int need = fmt(out, sizeof(out), ESP_LOG_WARN, "zigbee", NULL);

    TEST_ASSERT_EQUAL_INT(need, fmt(out, (size_t) need + 1, ESP_LOG_WARN, "zigbee", NULL));
    TEST_ASSERT_EQUAL_INT(-1, fmt(out, (size_t) need, ESP_LOG_WARN, "zigbee", NULL));
    TEST_ASSERT_EQUAL_INT(-1, fmt(out, 0, ESP_LOG_WARN, "zigbee", NULL));
}
