// radio_console_proto: one test per effect of the plan's BDD tree (docs/superpowers/plans/2026-11-13-flasher-radio-serial.md).
#include <string.h>
#include "unity.h"
#include "radio_console_proto.h"

/* ---- helpers ---- */

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* A frame as the page sends it; returns its length. */
static size_t make_frame(uint8_t *out, uint32_t offset, const uint8_t *payload, uint16_t len)
{
    out[0] = 'R';
    out[1] = 'F';
    put_u32(out + 2, offset);
    out[6] = (uint8_t)len;
    out[7] = (uint8_t)(len >> 8);
    if (len) {
        memcpy(out + 8, payload, len);
    }
    put_u32(out + 8 + len, radio_console_proto_crc32(0, out + 2, 6 + len));
    return 8 + len + 4;
}

static radio_console_proto_event_t feed_all(radio_console_proto_parser_t *p, const uint8_t *bytes, size_t n,
                                            uint32_t now, size_t *events)
{
    radio_console_proto_event_t last = RADIO_CONSOLE_PROTO_NONE;
    *events = 0;
    for (size_t i = 0; i < n; i++) {
        const radio_console_proto_event_t e = radio_console_proto_feed(p, bytes[i], now);
        if (e != RADIO_CONSOLE_PROTO_NONE) {
            last = e;
            (*events)++;
        }
    }
    return last;
}

static uint8_t frame_buf[RADIO_CONSOLE_PROTO_HEADER_LEN + RADIO_CONSOLE_PROTO_MAX_CHUNK + RADIO_CONSOLE_PROTO_CRC_LEN + 64];
static const uint8_t DATA[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};

/* ---- crc32 ---- */

TEST_CASE("crc32 on the check string 123456789: returns 0xCBF43926", "[radio_console]")
{
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926u, radio_console_proto_crc32(0, (const uint8_t *)"123456789", 9));
}

TEST_CASE("crc32 fed in two parts: equals the crc of the whole", "[radio_console]")
{
    const uint32_t part = radio_console_proto_crc32(0, (const uint8_t *)"1234", 4);
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926u, radio_console_proto_crc32(part, (const uint8_t *)"56789", 5));
}

/* ---- parse_load ---- */

TEST_CASE("parse_load on size and crc: returns ESP_OK with size and crc", "[radio_console]")
{
    char *argv[] = {"radio-fw", "load", "277824", "cbf43926"};
    size_t size = 0;
    uint32_t crc = 0;
    TEST_ASSERT_EQUAL(ESP_OK, radio_console_proto_parse_load(4, argv, &size, &crc));
    TEST_ASSERT_EQUAL(277824, size);
    TEST_ASSERT_EQUAL_HEX32(0xcbf43926u, crc);
}

TEST_CASE("parse_load on a missing argument: returns ESP_ERR_INVALID_ARG", "[radio_console]")
{
    char *argv[] = {"radio-fw", "load", "100"};
    size_t size;
    uint32_t crc;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, radio_console_proto_parse_load(3, argv, &size, &crc));
}

TEST_CASE("parse_load on a non-numeric argument: returns ESP_ERR_INVALID_ARG", "[radio_console]")
{
    char *size_junk[] = {"radio-fw", "load", "12k", "1"};
    char *crc_junk[] = {"radio-fw", "load", "12", "xyz"};
    size_t size;
    uint32_t crc;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, radio_console_proto_parse_load(4, size_junk, &size, &crc));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, radio_console_proto_parse_load(4, crc_junk, &size, &crc));
}

TEST_CASE("parse_load on a negative size: returns ESP_ERR_INVALID_ARG", "[radio_console]")
{
    char *argv[] = {"radio-fw", "load", "-5", "1"};
    size_t size;
    uint32_t crc;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, radio_console_proto_parse_load(4, argv, &size, &crc));
}

TEST_CASE("parse_load on size 0 or above the slot: returns ESP_ERR_INVALID_SIZE", "[radio_console]")
{
    char *zero[] = {"radio-fw", "load", "0", "1"};
    char *big[] = {"radio-fw", "load", "1572865", "1"};
    char *fits[] = {"radio-fw", "load", "1572864", "1"};
    size_t size;
    uint32_t crc;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, radio_console_proto_parse_load(4, zero, &size, &crc));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, radio_console_proto_parse_load(4, big, &size, &crc));
    TEST_ASSERT_EQUAL(ESP_OK, radio_console_proto_parse_load(4, fits, &size, &crc));
}

/* ---- frame decoding ---- */

TEST_CASE("feed on one whole valid frame: reports FRAME with offset, length and payload", "[radio_console]")
{
    radio_console_proto_parser_t p;
    size_t events;
    radio_console_proto_parser_init(&p);
    const size_t n = make_frame(frame_buf, 4096, DATA, sizeof(DATA));
    TEST_ASSERT_EQUAL(RADIO_CONSOLE_PROTO_FRAME, feed_all(&p, frame_buf, n, 0, &events));
    TEST_ASSERT_EQUAL(1, events);
    TEST_ASSERT_EQUAL(4096, p.offset);
    TEST_ASSERT_EQUAL(sizeof(DATA), p.len);
    TEST_ASSERT_EQUAL_MEMORY(DATA, p.payload, sizeof(DATA));
}

TEST_CASE("feed on a full-size frame: reports FRAME", "[radio_console]")
{
    static uint8_t big[RADIO_CONSOLE_PROTO_MAX_CHUNK];
    radio_console_proto_parser_t p;
    size_t events;
    memset(big, 0x5a, sizeof(big));
    radio_console_proto_parser_init(&p);
    const size_t n = make_frame(frame_buf, 0, big, sizeof(big));
    TEST_ASSERT_EQUAL(RADIO_CONSOLE_PROTO_FRAME, feed_all(&p, frame_buf, n, 0, &events));
    TEST_ASSERT_EQUAL(RADIO_CONSOLE_PROTO_MAX_CHUNK, p.len);
}

TEST_CASE("feed on a frame split across many feeds: reports nothing until the last byte, then FRAME", "[radio_console]")
{
    radio_console_proto_parser_t p;
    radio_console_proto_parser_init(&p);
    const size_t n = make_frame(frame_buf, 0, DATA, sizeof(DATA));
    for (size_t i = 0; i + 1 < n; i++) {
        TEST_ASSERT_EQUAL(RADIO_CONSOLE_PROTO_NONE, radio_console_proto_feed(&p, frame_buf[i], (uint32_t)i));
        TEST_ASSERT_EQUAL(RADIO_CONSOLE_PROTO_NONE, radio_console_proto_poll(&p, (uint32_t)i));
    }
    TEST_ASSERT_EQUAL(RADIO_CONSOLE_PROTO_FRAME, radio_console_proto_feed(&p, frame_buf[n - 1], (uint32_t)n));
}

TEST_CASE("feed on bytes before the magic: skips them and still decodes the frame", "[radio_console]")
{
    radio_console_proto_parser_t p;
    size_t events;
    uint8_t stream[sizeof(frame_buf)];
    const char junk[] = "\r\nradio-fw load 10 0\r\nRRx R";
    radio_console_proto_parser_init(&p);
    memcpy(stream, junk, sizeof(junk) - 1);
    const size_t n = make_frame(stream + sizeof(junk) - 1, 0, DATA, sizeof(DATA));
    TEST_ASSERT_EQUAL(RADIO_CONSOLE_PROTO_FRAME, feed_all(&p, stream, sizeof(junk) - 1 + n, 0, &events));
    TEST_ASSERT_EQUAL(1, events);
    TEST_ASSERT_EQUAL_MEMORY(DATA, p.payload, sizeof(DATA));
}

TEST_CASE("feed on a CRC mismatch: reports NAK_CRC", "[radio_console]")
{
    radio_console_proto_parser_t p;
    size_t events;
    radio_console_proto_parser_init(&p);
    const size_t n = make_frame(frame_buf, 0, DATA, sizeof(DATA));
    frame_buf[10] ^= 0xff;
    TEST_ASSERT_EQUAL(RADIO_CONSOLE_PROTO_NAK_CRC, feed_all(&p, frame_buf, n, 0, &events));
    TEST_ASSERT_EQUAL(1, events);
}

TEST_CASE("feed on a length above 1024: reports NAK_LENGTH without waiting for a payload", "[radio_console]")
{
    radio_console_proto_parser_t p;
    size_t events;
    const uint8_t header[] = {'R', 'F', 0, 0, 0, 0, 0x01, 0x04}; // 1025
    radio_console_proto_parser_init(&p);
    TEST_ASSERT_EQUAL(RADIO_CONSOLE_PROTO_NAK_LENGTH, feed_all(&p, header, sizeof(header), 0, &events));
    TEST_ASSERT_EQUAL(1, events);
}

TEST_CASE("feed on length 0: reports ABORT", "[radio_console]")
{
    radio_console_proto_parser_t p;
    size_t events;
    radio_console_proto_parser_init(&p);
    const size_t n = make_frame(frame_buf, 0, NULL, 0);
    TEST_ASSERT_EQUAL(RADIO_CONSOLE_PROTO_ABORT, feed_all(&p, frame_buf, n, 0, &events));
}

TEST_CASE("poll on more than 500 ms between two bytes of a frame: reports NAK_TIMEOUT and starts over at the magic",
          "[radio_console]")
{
    radio_console_proto_parser_t p;
    size_t events;
    radio_console_proto_parser_init(&p);
    const size_t n = make_frame(frame_buf, 0, DATA, sizeof(DATA));
    feed_all(&p, frame_buf, 5, 1000, &events);
    TEST_ASSERT_EQUAL(RADIO_CONSOLE_PROTO_NONE, radio_console_proto_poll(&p, 1500));
    TEST_ASSERT_EQUAL(RADIO_CONSOLE_PROTO_NAK_TIMEOUT, radio_console_proto_poll(&p, 1501));
    TEST_ASSERT_EQUAL(RADIO_CONSOLE_PROTO_NONE, radio_console_proto_poll(&p, 5000)); // reported once
    TEST_ASSERT_EQUAL(RADIO_CONSOLE_PROTO_FRAME, feed_all(&p, frame_buf, n, 5000, &events));
}

TEST_CASE("poll with no partial frame: reports nothing", "[radio_console]")
{
    radio_console_proto_parser_t p;
    radio_console_proto_parser_init(&p);
    TEST_ASSERT_EQUAL(RADIO_CONSOLE_PROTO_NONE, radio_console_proto_poll(&p, 100000));
}

/* ---- accept ---- */

TEST_CASE("accept on offset == expected inside the image: returns WRITE, next = offset + length", "[radio_console]")
{
    size_t next = 0;
    TEST_ASSERT_EQUAL(RADIO_CONSOLE_PROTO_WRITE, radio_console_proto_accept(1024, 2000, 1024, 976, &next));
    TEST_ASSERT_EQUAL(2000, next);
}

TEST_CASE("accept on offset < expected: returns REPEAT, next = expected", "[radio_console]")
{
    size_t next = 0;
    TEST_ASSERT_EQUAL(RADIO_CONSOLE_PROTO_REPEAT, radio_console_proto_accept(2048, 4096, 1024, 1024, &next));
    TEST_ASSERT_EQUAL(2048, next);
}

TEST_CASE("accept on offset > expected or past the image: returns NAK_OFFSET with expected", "[radio_console]")
{
    size_t next = 0;
    TEST_ASSERT_EQUAL(RADIO_CONSOLE_PROTO_NAK_OFFSET, radio_console_proto_accept(1024, 4096, 2048, 1024, &next));
    TEST_ASSERT_EQUAL(1024, next);
    TEST_ASSERT_EQUAL(RADIO_CONSOLE_PROTO_NAK_OFFSET, radio_console_proto_accept(1024, 1500, 1024, 1024, &next));
    TEST_ASSERT_EQUAL(1024, next);
}

/* ---- state ---- */

TEST_CASE("state on a maintenance running: returns busy", "[radio_console]")
{
    const radio_console_proto_state_input_t in = {.maintenance = true, .updater_reachable = true};
    TEST_ASSERT_EQUAL_STRING("busy", radio_console_proto_state(&in));
}

TEST_CASE("state on a reachable updater: returns updater", "[radio_console]")
{
    const radio_console_proto_state_input_t in = {.updater_reachable = true};
    TEST_ASSERT_EQUAL_STRING("updater", radio_console_proto_state(&in));
}

TEST_CASE("state on a probe with esp_hosted: returns esp_hosted", "[radio_console]")
{
    const radio_console_proto_state_input_t in = {.esp_hosted = true, .failed = true};
    TEST_ASSERT_EQUAL_STRING("esp_hosted", radio_console_proto_state(&in));
}

TEST_CASE("state on nothing answering: returns fault", "[radio_console]")
{
    const radio_console_proto_state_input_t in = {.failed = true};
    TEST_ASSERT_EQUAL_STRING("fault", radio_console_proto_state(&in));
}

TEST_CASE("state on no probe yet: returns probing", "[radio_console]")
{
    const radio_console_proto_state_input_t in = {0};
    TEST_ASSERT_EQUAL_STRING("probing", radio_console_proto_state(&in));
}

/* ---- reply lines ---- */

TEST_CASE("line on a kind and fields: writes one line with prefix and newline", "[radio_console]")
{
    char buf[RADIO_CONSOLE_PROTO_LINE_MAX];
    const size_t n = radio_console_proto_line(buf, sizeof(buf), "ack", "2048");
    TEST_ASSERT_EQUAL_STRING("@radio-fw ack 2048\n", buf);
    TEST_ASSERT_EQUAL(strlen(buf), n);
}

TEST_CASE("line on a kind without fields: writes the kind alone", "[radio_console]")
{
    char buf[RADIO_CONSOLE_PROTO_LINE_MAX];
    radio_console_proto_line(buf, sizeof(buf), "received", NULL);
    TEST_ASSERT_EQUAL_STRING("@radio-fw received\n", buf);
}

TEST_CASE("line on a detail longer than the buffer: truncates the detail and keeps the newline", "[radio_console]")
{
    char buf[32];
    const size_t n = radio_console_proto_line(buf, sizeof(buf), "error", "install_failed a very long detail text here");
    TEST_ASSERT_EQUAL(sizeof(buf) - 1, n);
    TEST_ASSERT_EQUAL('\n', buf[n - 1]);
    TEST_ASSERT_EQUAL_STRING_LEN("@radio-fw error install_failed", buf, 30);
}

TEST_CASE("line on a detail with a newline: replaces it with a space", "[radio_console]")
{
    char buf[RADIO_CONSOLE_PROTO_LINE_MAX];
    radio_console_proto_line(buf, sizeof(buf), "error", "push_failed one\ntwo\r");
    TEST_ASSERT_EQUAL_STRING("@radio-fw error push_failed one two \n", buf);
}

TEST_CASE("line on a buffer too small for the kind: writes nothing", "[radio_console]")
{
    char buf[8] = "x";
    TEST_ASSERT_EQUAL(0, radio_console_proto_line(buf, sizeof(buf), "received", NULL));
    TEST_ASSERT_EQUAL_STRING("", buf);
}

TEST_CASE("status_line on an updater with an RCP: writes state, installed, base and jumper", "[radio_console]")
{
    char buf[RADIO_CONSOLE_PROTO_LINE_MAX];
    radio_console_proto_status_line(buf, sizeof(buf), "updater", "20261111T000000Z-abc", "base-1a2b3c4d", true);
    TEST_ASSERT_EQUAL_STRING("@radio-fw status state=updater installed=20261111T000000Z-abc base=base-1a2b3c4d jumper=1\n",
                             buf);
}

TEST_CASE("status_line on empty versions: writes - for each", "[radio_console]")
{
    char buf[RADIO_CONSOLE_PROTO_LINE_MAX];
    radio_console_proto_status_line(buf, sizeof(buf), "esp_hosted", "", NULL, false);
    TEST_ASSERT_EQUAL_STRING("@radio-fw status state=esp_hosted installed=- base=- jumper=0\n", buf);
}

TEST_CASE("result_line on ESP_OK after an RCP: is done with the installed version", "[radio_console]")
{
    char buf[RADIO_CONSOLE_PROTO_LINE_MAX];
    radio_console_proto_result_line(buf, sizeof(buf), ESP_OK, NULL, "", "v0.7.0");
    TEST_ASSERT_EQUAL_STRING("@radio-fw done installed=v0.7.0\n", buf);
}

TEST_CASE("result_line on ESP_OK after the installer: is done with installed -", "[radio_console]")
{
    char buf[RADIO_CONSOLE_PROTO_LINE_MAX];
    radio_console_proto_result_line(buf, sizeof(buf), ESP_OK, NULL, "", "");
    TEST_ASSERT_EQUAL_STRING("@radio-fw done installed=-\n", buf);
}

TEST_CASE("result_line on a refused installer: is error with the code and the push's text", "[radio_console]")
{
    char buf[RADIO_CONSOLE_PROTO_LINE_MAX];
    radio_console_proto_result_line(buf, sizeof(buf), ESP_FAIL, "invalid_image",
                                    "wrong c6_installer.bin: base-11111111, need base-22222222", NULL);
    TEST_ASSERT_EQUAL_STRING("@radio-fw error invalid_image wrong c6_installer.bin: base-11111111, need base-22222222\n",
                             buf);
}

TEST_CASE("result_line on an error without detail: falls back to the error name", "[radio_console]")
{
    char buf[RADIO_CONSOLE_PROTO_LINE_MAX];
    radio_console_proto_result_line(buf, sizeof(buf), ESP_ERR_TIMEOUT, "install_failed", "", NULL);
    TEST_ASSERT_EQUAL_STRING("@radio-fw error install_failed ESP_ERR_TIMEOUT\n", buf);
}
