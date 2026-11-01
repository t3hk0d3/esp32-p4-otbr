// C6 console capture: ring and line forwarding (spec 5.10).
#include <string.h>
#include "unity.h"
#include "log_ring.h"

#define RING_CAP 16

static uint8_t storage[RING_CAP];
static log_ring_t ring;
static char lines[8][LOG_RING_LINE_MAX + 1];
static int line_count;

static void on_line(void *ctx, const char *line)
{
    if (line_count < 8) {
        strcpy(lines[line_count], line);
    }
    line_count++;
}

static void setup(void)
{
    memset(storage, 0, sizeof(storage));
    line_count = 0;
    log_ring_init(&ring, storage, RING_CAP, on_line, NULL);
}

static void push_text(const char *text)
{
    log_ring_push(&ring, (const uint8_t *)text, strlen(text));
}

TEST_CASE("On console bytes: appends them to the ring and reads them back in order", "[rcp_sync][log]")
{
    char out[RING_CAP + 1] = "";

    setup();
    push_text("hello\nwor");
    TEST_ASSERT_EQUAL(9, log_ring_read(&ring, out, sizeof(out)));
    TEST_ASSERT_EQUAL_MEMORY("hello\nwor", out, 9);
}

TEST_CASE("On more bytes than the ring holds: keeps the newest ones", "[rcp_sync][log]")
{
    char out[RING_CAP + 1] = "";

    setup();
    push_text("0123456789");
    push_text("abcdefghij"); /* 20 bytes into 16 */
    TEST_ASSERT_EQUAL(RING_CAP, log_ring_read(&ring, out, sizeof(out)));
    TEST_ASSERT_EQUAL_MEMORY("456789abcdefghij", out, RING_CAP);
}

TEST_CASE("On a read buffer smaller than the content: returns the newest bytes that fit", "[rcp_sync][log]")
{
    char out[4];

    setup();
    push_text("abcdefgh");
    TEST_ASSERT_EQUAL(4, log_ring_read(&ring, out, sizeof(out)));
    TEST_ASSERT_EQUAL_MEMORY("efgh", out, 4);
}

TEST_CASE("On an empty ring: reads nothing", "[rcp_sync][log]")
{
    char out[4];

    setup();
    TEST_ASSERT_EQUAL(0, log_ring_read(&ring, out, sizeof(out)));
}

TEST_CASE("On a complete line: forwards it without its line end", "[rcp_sync][log]")
{
    setup();
    push_text("I (123) boot: ok\r\n");
    TEST_ASSERT_EQUAL(1, line_count);
    TEST_ASSERT_EQUAL_STRING("I (123) boot: ok", lines[0]);
}

TEST_CASE("On a partial line: forwards nothing until its end arrives", "[rcp_sync][log]")
{
    setup();
    push_text("par");
    TEST_ASSERT_EQUAL(0, line_count);
    push_text("tial\n");
    TEST_ASSERT_EQUAL(1, line_count);
    TEST_ASSERT_EQUAL_STRING("partial", lines[0]);
}

TEST_CASE("On several lines in one chunk: forwards each, skipping empty ones", "[rcp_sync][log]")
{
    setup();
    push_text("a\n\r\nb\n");
    TEST_ASSERT_EQUAL(2, line_count);
    TEST_ASSERT_EQUAL_STRING("a", lines[0]);
    TEST_ASSERT_EQUAL_STRING("b", lines[1]);
}

TEST_CASE("On ANSI colour sequences: removes them from the forwarded line but keeps the ring raw", "[rcp_sync][log]")
{
    char out[RING_CAP + 1] = "";

    setup();
    push_text("\x1b[0;32mok\x1b[0m\n");
    TEST_ASSERT_EQUAL(1, line_count);
    TEST_ASSERT_EQUAL_STRING("ok", lines[0]);
    TEST_ASSERT_EQUAL(14, log_ring_read(&ring, out, sizeof(out)));
    TEST_ASSERT_EQUAL_HEX8(0x1b, out[0]);
}

TEST_CASE("On a line longer than the limit: forwards it in pieces", "[rcp_sync][log]")
{
    uint8_t big[LOG_RING_LINE_MAX + 10];

    setup();
    memset(big, 'x', sizeof(big));
    log_ring_push(&ring, big, sizeof(big));
    TEST_ASSERT_EQUAL(1, line_count); /* the first full piece */
    TEST_ASSERT_EQUAL(LOG_RING_LINE_MAX, strlen(lines[0]));
    push_text("\n");
    TEST_ASSERT_EQUAL(2, line_count);
    TEST_ASSERT_EQUAL(10, strlen(lines[1]));
}

TEST_CASE("On other control characters: drops them from the forwarded line", "[rcp_sync][log]")
{
    setup();
    push_text("a\x01\x02" "b\n");
    TEST_ASSERT_EQUAL_STRING("ab", lines[0]);
}

TEST_CASE("On no line callback: still fills the ring", "[rcp_sync][log]")
{
    char out[8];

    log_ring_init(&ring, storage, RING_CAP, NULL, NULL);
    push_text("abc\n");
    TEST_ASSERT_EQUAL(4, log_ring_read(&ring, out, sizeof(out)));
}

TEST_CASE("On the ROM boot banner line: is recognised as the jumper banner", "[rcp_sync][log]")
{
    TEST_ASSERT_TRUE(log_ring_is_rom_banner("ESP-ROM:esp32c6-20220919"));
    TEST_ASSERT_TRUE(log_ring_is_rom_banner("garbage ESP-ROM:esp32c6-20220919"));
}

TEST_CASE("On any other line: is not the jumper banner", "[rcp_sync][log]")
{
    TEST_ASSERT_FALSE(log_ring_is_rom_banner("I (30) boot: ESP-IDF v6.1"));
    TEST_ASSERT_FALSE(log_ring_is_rom_banner(""));
    TEST_ASSERT_FALSE(log_ring_is_rom_banner(NULL));
}
