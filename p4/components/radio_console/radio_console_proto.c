#include "radio_console_proto.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAGIC_0 'R'
#define MAGIC_1 'F'

uint32_t radio_console_proto_crc32(uint32_t crc, const uint8_t *data, size_t len)
{
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

/* A whole unsigned number in `base`, nothing else; false on empty text, a sign, junk or overflow. */
static bool parse_number(const char *text, int base, unsigned long long max, unsigned long long *out)
{
    if (!text || text[0] == '\0' || text[0] == '-' || text[0] == '+' || text[0] == ' ') {
        return false;
    }
    char *end = NULL;
    errno = 0;
    const unsigned long long value = strtoull(text, &end, base);
    if (errno != 0 || *end != '\0' || value > max) {
        return false;
    }
    *out = value;
    return true;
}

esp_err_t radio_console_proto_parse_load(int argc, char **argv, size_t *size, uint32_t *crc)
{
    unsigned long long n, c;

    if (argc != 4 || !argv || !size || !crc || !parse_number(argv[2], 10, UINT32_MAX, &n) ||
            !parse_number(argv[3], 16, UINT32_MAX, &c)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (n == 0 || n > RADIO_CONSOLE_PROTO_MAX_IMAGE) {
        return ESP_ERR_INVALID_SIZE;
    }
    *size = (size_t)n;
    *crc = (uint32_t)c;
    return ESP_OK;
}

void radio_console_proto_parser_init(radio_console_proto_parser_t *p)
{
    memset(p, 0, sizeof(*p));
}

static uint32_t get_u32(const uint8_t *b)
{
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static uint16_t frame_len(const radio_console_proto_parser_t *p)
{
    return (uint16_t)(p->buf[6] | (p->buf[7] << 8));
}

radio_console_proto_event_t radio_console_proto_feed(radio_console_proto_parser_t *p, uint8_t byte, uint32_t now_ms)
{
    if (p->have == 0) {
        if (byte == MAGIC_0) {
            p->buf[p->have++] = byte;
            p->last_ms = now_ms;
        }
        return RADIO_CONSOLE_PROTO_NONE;
    }
    if (p->have == 1) {
        if (byte == MAGIC_1) {
            p->buf[p->have++] = byte;
            p->last_ms = now_ms;
        } else {
            p->have = byte == MAGIC_0 ? 1 : 0; // "RR F": the second R may start the frame
            p->last_ms = now_ms;
        }
        return RADIO_CONSOLE_PROTO_NONE;
    }
    p->buf[p->have++] = byte;
    p->last_ms = now_ms;
    if (p->have < RADIO_CONSOLE_PROTO_HEADER_LEN) {
        return RADIO_CONSOLE_PROTO_NONE;
    }
    const uint16_t len = frame_len(p);
    if (len > RADIO_CONSOLE_PROTO_MAX_CHUNK) {
        p->have = 0;
        return RADIO_CONSOLE_PROTO_NAK_LENGTH;
    }
    const size_t total = RADIO_CONSOLE_PROTO_HEADER_LEN + len + RADIO_CONSOLE_PROTO_CRC_LEN;
    if (p->have < total) {
        return RADIO_CONSOLE_PROTO_NONE;
    }
    p->have = 0;
    const size_t covered = RADIO_CONSOLE_PROTO_HEADER_LEN - 2 + len; // offset, length, payload
    if (radio_console_proto_crc32(0, p->buf + 2, covered) != get_u32(p->buf + 2 + covered)) {
        return RADIO_CONSOLE_PROTO_NAK_CRC;
    }
    if (len == 0) {
        return RADIO_CONSOLE_PROTO_ABORT;
    }
    p->offset = get_u32(p->buf + 2);
    p->len = len;
    p->payload = p->buf + RADIO_CONSOLE_PROTO_HEADER_LEN;
    return RADIO_CONSOLE_PROTO_FRAME;
}

radio_console_proto_event_t radio_console_proto_poll(radio_console_proto_parser_t *p, uint32_t now_ms)
{
    if (p->have > 0 && now_ms - p->last_ms > RADIO_CONSOLE_PROTO_BYTE_TIMEOUT_MS) {
        p->have = 0;
        return RADIO_CONSOLE_PROTO_NAK_TIMEOUT;
    }
    return RADIO_CONSOLE_PROTO_NONE;
}

radio_console_proto_accept_t radio_console_proto_accept(size_t expected, size_t size, uint32_t offset, uint16_t len,
                                                        size_t *next)
{
    *next = expected;
    if (offset < expected) {
        return RADIO_CONSOLE_PROTO_REPEAT;
    }
    if (offset > expected || (size_t)offset + len > size) {
        return RADIO_CONSOLE_PROTO_NAK_OFFSET;
    }
    *next = (size_t)offset + len;
    return RADIO_CONSOLE_PROTO_WRITE;
}

const char *radio_console_proto_state(const radio_console_proto_state_input_t *in)
{
    if (in->maintenance) {
        return "busy";
    }
    if (in->updater_reachable) {
        return "updater";
    }
    if (in->esp_hosted) {
        return "esp_hosted";
    }
    return in->failed ? "fault" : "probing";
}

size_t radio_console_proto_line(char *buf, size_t cap, const char *kind, const char *fields)
{
    const size_t prefix = strlen(RADIO_CONSOLE_PROTO_PREFIX);
    const size_t kind_len = strlen(kind);

    if (!buf || cap < prefix + kind_len + 2) { // "\n" and NUL
        if (buf && cap > 0) {
            buf[0] = '\0';
        }
        return 0;
    }
    size_t n = 0;
    memcpy(buf, RADIO_CONSOLE_PROTO_PREFIX, prefix);
    n += prefix;
    memcpy(buf + n, kind, kind_len);
    n += kind_len;
    if (fields && fields[0] != '\0' && n + 3 < cap) { // room for " x\n\0"
        buf[n++] = ' ';
        for (const char *c = fields; *c && n + 2 < cap; c++) {
            buf[n++] = (*c == '\n' || *c == '\r') ? ' ' : *c;
        }
    }
    buf[n++] = '\n';
    buf[n] = '\0';
    return n;
}

static const char *or_dash(const char *version)
{
    return version && version[0] ? version : "-";
}

size_t radio_console_proto_status_line(char *buf, size_t cap, const char *state, const char *installed,
                                       const char *base, bool jumper)
{
    char fields[RADIO_CONSOLE_PROTO_LINE_MAX];

    snprintf(fields, sizeof(fields), "state=%s installed=%s base=%s jumper=%d", state, or_dash(installed),
             or_dash(base), jumper ? 1 : 0);
    return radio_console_proto_line(buf, cap, "status", fields);
}

size_t radio_console_proto_result_line(char *buf, size_t cap, esp_err_t err, const char *code, const char *detail,
                                       const char *installed)
{
    char fields[RADIO_CONSOLE_PROTO_LINE_MAX];

    if (err == ESP_OK) {
        snprintf(fields, sizeof(fields), "installed=%s", or_dash(installed));
        return radio_console_proto_line(buf, cap, "done", fields);
    }
    snprintf(fields, sizeof(fields), "%s %s", code && code[0] ? code : "push_failed",
             detail && detail[0] ? detail : esp_err_to_name(err));
    return radio_console_proto_line(buf, cap, "error", fields);
}
