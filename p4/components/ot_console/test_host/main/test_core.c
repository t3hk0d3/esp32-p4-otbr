#include <stdlib.h>
#include <string.h>
#include "unity.h"
#include "unity_test_runner.h"
#include "ot_console_core.h"

#define MAX_LINE 383

static ot_console_line_t normalise(char *buf, size_t cap, const char *in)
{
    strlcpy(buf, in, cap);
    return ot_console_line_normalise(buf, MAX_LINE);
}

static ot_console_arb_t running_arb(void)
{
    ot_console_arb_t a;

    ot_console_arb_init(&a);
    ot_console_arb_stack(&a, true);
    return a;
}

static ot_console_dest_t out(ot_console_arb_t *a, const char *text, ot_console_span_t *span)
{
    return ot_console_arb_output(a, text, strlen(text), span);
}

/* ---- ot_console_line_normalise ---- */

TEST_CASE("Plain command: returns OK, line unchanged", "[ot_console]")
{
    char buf[64];

    TEST_ASSERT_EQUAL(OT_CONSOLE_LINE_OK, normalise(buf, sizeof(buf), "router table"));
    TEST_ASSERT_EQUAL_STRING("router table", buf);
}

TEST_CASE("Trailing CR LF or LF: stripped", "[ot_console]")
{
    char buf[64];

    TEST_ASSERT_EQUAL(OT_CONSOLE_LINE_OK, normalise(buf, sizeof(buf), "state\r\n"));
    TEST_ASSERT_EQUAL_STRING("state", buf);
    TEST_ASSERT_EQUAL(OT_CONSOLE_LINE_OK, normalise(buf, sizeof(buf), "state\n"));
    TEST_ASSERT_EQUAL_STRING("state", buf);
}

TEST_CASE("Leading ot prefix: stripped with the blanks after it", "[ot_console]")
{
    char buf[64];

    TEST_ASSERT_EQUAL(OT_CONSOLE_LINE_OK, normalise(buf, sizeof(buf), "ot state"));
    TEST_ASSERT_EQUAL_STRING("state", buf);
    TEST_ASSERT_EQUAL(OT_CONSOLE_LINE_OK, normalise(buf, sizeof(buf), "  ot   dataset active -x\n"));
    TEST_ASSERT_EQUAL_STRING("dataset active -x", buf);
}

TEST_CASE("Command starting with ot but no blank: kept", "[ot_console]")
{
    char buf[64];

    TEST_ASSERT_EQUAL(OT_CONSOLE_LINE_OK, normalise(buf, sizeof(buf), "otstate"));
    TEST_ASSERT_EQUAL_STRING("otstate", buf);
}

TEST_CASE("Empty or blank line: returns EMPTY", "[ot_console]")
{
    char buf[64];

    TEST_ASSERT_EQUAL(OT_CONSOLE_LINE_EMPTY, normalise(buf, sizeof(buf), ""));
    TEST_ASSERT_EQUAL(OT_CONSOLE_LINE_EMPTY, normalise(buf, sizeof(buf), "  \r\n"));
    TEST_ASSERT_EQUAL(OT_CONSOLE_LINE_EMPTY, normalise(buf, sizeof(buf), "ot "));
}

TEST_CASE("Line over the maximum: returns TOO_LONG", "[ot_console]")
{
    char buf[512];
    char in[512];

    memset(in, 'a', MAX_LINE + 1);
    in[MAX_LINE + 1] = '\0';
    TEST_ASSERT_EQUAL(OT_CONSOLE_LINE_TOO_LONG, normalise(buf, sizeof(buf), in));
    in[MAX_LINE] = '\n';
    TEST_ASSERT_EQUAL(OT_CONSOLE_LINE_OK, normalise(buf, sizeof(buf), in));
}

/* ---- ot_console_arb_submit ---- */

TEST_CASE("Line with nothing pending, running: SEND, recorded as pending", "[ot_console]")
{
    ot_console_arb_t a = running_arb();

    TEST_ASSERT_EQUAL(OT_CONSOLE_SEND, ot_console_arb_submit(&a, OT_CONSOLE_SRC_WS, "state"));
    TEST_ASSERT_EQUAL_STRING("state", ot_console_arb_pending(&a));
}

TEST_CASE("Second line while one is pending: BUSY, first kept", "[ot_console]")
{
    ot_console_arb_t a = running_arb();

    ot_console_arb_submit(&a, OT_CONSOLE_SRC_WS, "ping fd00::1");
    TEST_ASSERT_EQUAL(OT_CONSOLE_BUSY, ot_console_arb_submit(&a, OT_CONSOLE_SRC_WS, "state"));
    TEST_ASSERT_EQUAL_STRING("ping fd00::1", ot_console_arb_pending(&a));
}

TEST_CASE("factoryreset while pending: is sent", "[ot_console]")
{
    ot_console_arb_t a = running_arb();

    ot_console_arb_submit(&a, OT_CONSOLE_SRC_WS, "ping fd00::1");
    TEST_ASSERT_EQUAL(OT_CONSOLE_SEND, ot_console_arb_submit(&a, OT_CONSOLE_SRC_WS, "factoryreset"));
}

TEST_CASE("Line while the stack is not running: NOT_RUNNING, nothing recorded", "[ot_console]")
{
    ot_console_arb_t a;

    ot_console_arb_init(&a);
    TEST_ASSERT_EQUAL(OT_CONSOLE_NOT_RUNNING, ot_console_arb_submit(&a, OT_CONSOLE_SRC_WS, "state"));
    TEST_ASSERT_NULL(ot_console_arb_pending(&a));
}

TEST_CASE("USB line while a session is attached: IN_USE", "[ot_console]")
{
    ot_console_arb_t a = running_arb();

    ot_console_arb_attach(&a, true);
    TEST_ASSERT_EQUAL(OT_CONSOLE_IN_USE, ot_console_arb_submit(&a, OT_CONSOLE_SRC_USB, "state"));
    TEST_ASSERT_NULL(ot_console_arb_pending(&a));
}

/* ---- ot_console_arb_output ---- */

TEST_CASE("Output while a WebSocket line is pending: to the session", "[ot_console]")
{
    ot_console_arb_t a = running_arb();
    ot_console_span_t span;

    ot_console_arb_attach(&a, true);
    ot_console_arb_submit(&a, OT_CONSOLE_SRC_WS, "state");
    TEST_ASSERT_EQUAL(OT_CONSOLE_TO_SESSION, out(&a, "router\r\n", &span));
    TEST_ASSERT_EQUAL(8, span.len);
    TEST_ASSERT_FALSE(span.flush_carry);
}

TEST_CASE("Output while a USB line is pending: to stdout", "[ot_console]")
{
    ot_console_arb_t a = running_arb();
    ot_console_span_t span;

    ot_console_arb_submit(&a, OT_CONSOLE_SRC_USB, "state");
    ot_console_arb_attach(&a, true);
    TEST_ASSERT_EQUAL(OT_CONSOLE_TO_STDOUT, out(&a, "router\r\n", &span));
}

TEST_CASE("Output with nothing pending and a session: to the session", "[ot_console]")
{
    ot_console_arb_t a = running_arb();
    ot_console_span_t span;

    ot_console_arb_attach(&a, true);
    TEST_ASSERT_EQUAL(OT_CONSOLE_TO_SESSION, out(&a, "Done\r\n", &span));
}

TEST_CASE("Output with nothing pending and no session: to stdout", "[ot_console]")
{
    ot_console_arb_t a = running_arb();
    ot_console_span_t span;

    TEST_ASSERT_EQUAL(OT_CONSOLE_TO_STDOUT, out(&a, "Done\r\n", &span));
}

TEST_CASE("Prompt: not forwarded, pending cleared", "[ot_console]")
{
    ot_console_arb_t a = running_arb();
    ot_console_span_t span;

    ot_console_arb_attach(&a, true);
    ot_console_arb_submit(&a, OT_CONSOLE_SRC_WS, "state");
    TEST_ASSERT_EQUAL(OT_CONSOLE_DROP_PROMPT, out(&a, "> ", &span));
    TEST_ASSERT_NULL(ot_console_arb_pending(&a));
    TEST_ASSERT_EQUAL(OT_CONSOLE_SEND, ot_console_arb_submit(&a, OT_CONSOLE_SRC_WS, "state"));
}

TEST_CASE("Prompt split across two writes: recognised, not forwarded", "[ot_console]")
{
    ot_console_arb_t a = running_arb();
    ot_console_span_t span;

    ot_console_arb_attach(&a, true);
    ot_console_arb_submit(&a, OT_CONSOLE_SRC_WS, "state");
    TEST_ASSERT_EQUAL(OT_CONSOLE_TO_SESSION, out(&a, ">", &span));
    TEST_ASSERT_EQUAL(0, span.len);
    TEST_ASSERT_EQUAL(OT_CONSOLE_DROP_PROMPT, out(&a, " ", &span));
    TEST_ASSERT_FALSE(span.flush_carry);
    TEST_ASSERT_NULL(ot_console_arb_pending(&a));
}

TEST_CASE("Held-back > followed by other text: released before it", "[ot_console]")
{
    ot_console_arb_t a = running_arb();
    ot_console_span_t span;

    ot_console_arb_attach(&a, true);
    ot_console_arb_submit(&a, OT_CONSOLE_SRC_WS, "state");
    TEST_ASSERT_EQUAL(OT_CONSOLE_TO_SESSION, out(&a, "a->", &span));
    TEST_ASSERT_EQUAL(2, span.len);
    TEST_ASSERT_FALSE(span.flush_carry);
    TEST_ASSERT_EQUAL(OT_CONSOLE_TO_SESSION, out(&a, "b\r\n", &span));
    TEST_ASSERT_TRUE(span.flush_carry);
    TEST_ASSERT_EQUAL(3, span.len);
    TEST_ASSERT_EQUAL_STRING("state", ot_console_arb_pending(&a));
}

/* ---- ot_console_arb_stack / attach ---- */

TEST_CASE("Stack stopping or restarting: pending cleared", "[ot_console]")
{
    ot_console_arb_t a = running_arb();

    ot_console_arb_submit(&a, OT_CONSOLE_SRC_WS, "ping fd00::1");
    ot_console_arb_stack(&a, false);
    TEST_ASSERT_NULL(ot_console_arb_pending(&a));
    ot_console_arb_stack(&a, true);
    ot_console_arb_submit(&a, OT_CONSOLE_SRC_WS, "ping fd00::1");
    ot_console_arb_stack(&a, true);
    TEST_ASSERT_NULL(ot_console_arb_pending(&a));
}

TEST_CASE("Detach while pending: pending kept until the prompt", "[ot_console]")
{
    ot_console_arb_t a = running_arb();
    ot_console_span_t span;

    ot_console_arb_attach(&a, true);
    ot_console_arb_submit(&a, OT_CONSOLE_SRC_WS, "ping fd00::1");
    ot_console_arb_attach(&a, false);
    TEST_ASSERT_EQUAL_STRING("ping fd00::1", ot_console_arb_pending(&a));
    TEST_ASSERT_EQUAL(OT_CONSOLE_DROP_PROMPT, out(&a, "> ", &span));
    TEST_ASSERT_NULL(ot_console_arb_pending(&a));
}

/* ---- ot_console_buf ---- */

TEST_CASE("Push within the cap: take returns the bytes in order", "[ot_console]")
{
    uint8_t storage[8];
    ot_console_buf_t b;
    char got[16] = {0};

    ot_console_buf_init(&b, storage, sizeof(storage));
    ot_console_buf_push(&b, "abc", 3);
    ot_console_buf_push(&b, "de", 2);
    TEST_ASSERT_EQUAL(5, ot_console_buf_take(&b, got, sizeof(got)));
    TEST_ASSERT_EQUAL_STRING("abcde", got);
    TEST_ASSERT_EQUAL(0, ot_console_buf_take_dropped(&b));
}

TEST_CASE("Push wrapping around the end: order kept", "[ot_console]")
{
    uint8_t storage[8];
    ot_console_buf_t b;
    char got[16] = {0};

    ot_console_buf_init(&b, storage, sizeof(storage));
    ot_console_buf_push(&b, "abcdef", 6);
    ot_console_buf_take(&b, got, 4);
    ot_console_buf_push(&b, "ghijk", 5);
    memset(got, 0, sizeof(got));
    TEST_ASSERT_EQUAL(7, ot_console_buf_take(&b, got, sizeof(got)));
    TEST_ASSERT_EQUAL_STRING("efghijk", got);
}

TEST_CASE("Push past the cap: oldest dropped, count reported once", "[ot_console]")
{
    uint8_t storage[8];
    ot_console_buf_t b;
    char got[16] = {0};

    ot_console_buf_init(&b, storage, sizeof(storage));
    ot_console_buf_push(&b, "abcdef", 6);
    ot_console_buf_push(&b, "ghij", 4);
    TEST_ASSERT_EQUAL(2, ot_console_buf_take_dropped(&b));
    TEST_ASSERT_EQUAL(0, ot_console_buf_take_dropped(&b));
    TEST_ASSERT_EQUAL(8, ot_console_buf_take(&b, got, sizeof(got)));
    TEST_ASSERT_EQUAL_STRING("cdefghij", got);
    ot_console_buf_push(&b, "0123456789AB", 12);
    TEST_ASSERT_EQUAL(4, ot_console_buf_take_dropped(&b));
    memset(got, 0, sizeof(got));
    ot_console_buf_take(&b, got, sizeof(got));
    TEST_ASSERT_EQUAL_STRING("456789AB", got);
}

TEST_CASE("Take into a smaller buffer: the rest stays", "[ot_console]")
{
    uint8_t storage[8];
    ot_console_buf_t b;
    char got[16] = {0};

    ot_console_buf_init(&b, storage, sizeof(storage));
    ot_console_buf_push(&b, "abcdef", 6);
    TEST_ASSERT_EQUAL(4, ot_console_buf_take(&b, got, 4));
    TEST_ASSERT_EQUAL(2, b.len);
}

/* ---- ot_console_notice_* ---- */

TEST_CASE("Each notice: exact text", "[ot_console]")
{
    char s[160];

    ot_console_notice_connected(s, sizeof(s), "esp-otbr-d2cafc", "OPENTHREAD/1.4", "router", 0x6800);
    TEST_ASSERT_EQUAL_STRING("# Connected to esp-otbr-d2cafc (OpenThread OPENTHREAD/1.4, router 0x6800)", s);
    ot_console_notice_not_running(s, sizeof(s), "SNIFFER");
    TEST_ASSERT_EQUAL_STRING("# Thread is not running (mode SNIFFER)", s);
    ot_console_notice_busy(s, sizeof(s), "ping fd00::1");
    TEST_ASSERT_EQUAL_STRING("# Busy: \"ping fd00::1\" is still running", s);
    ot_console_notice_taken_over(s, sizeof(s), "192.168.1.20");
    TEST_ASSERT_EQUAL_STRING("# Closed: a new console session opened from 192.168.1.20", s);
    ot_console_notice_too_long(s, sizeof(s), 383);
    TEST_ASSERT_EQUAL_STRING("# Line too long (max 383 bytes)", s);
    ot_console_notice_dropped(s, sizeof(s), 1024);
    TEST_ASSERT_EQUAL_STRING("# Output dropped (1024 bytes)", s);
    TEST_ASSERT_EQUAL(36, ot_console_notice_turned_off(s, sizeof(s)));
    TEST_ASSERT_EQUAL_STRING("# Closed: the console was turned off", s);
}

void app_main(void)
{
    UNITY_BEGIN();
    unity_run_all_tests();
    int failures = (int)UNITY_END();
    exit(failures == 0 ? 0 : 1);
}
