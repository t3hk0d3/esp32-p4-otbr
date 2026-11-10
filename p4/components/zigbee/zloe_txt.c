// ZLOE's mDNS records (zigbee_zloe_txt.h). Pure.
#include <stdio.h>
#include <string.h>

#include "zigbee_zloe_txt.h"

// EUI64s are written big-endian in TXT and kept in wire order (little-endian).
static void hex_of(const uint8_t wire[8], char out[17])
{
    for (int i = 0; i < 8; i++) {
        snprintf(&out[i * 2], 3, "%02x", wire[7 - i]);
    }
}

static int nibble(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

// `len` hex digits into `out`, big-endian. false: wrong length or not hex.
static bool parse_hex(const char *text, size_t len, uint8_t *out)
{
    if (text == NULL || strlen(text) != len) {
        return false;
    }
    for (size_t i = 0; i < len; i += 2) {
        const int hi = nibble(text[i]), lo = nibble(text[i + 1]);

        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i / 2] = (uint8_t)(hi << 4 | lo);
    }
    return true;
}

static bool parse_eui64(const char *text, uint8_t wire[8])
{
    uint8_t be[8];

    if (!parse_hex(text, 16, be)) {
        return false;
    }
    for (int i = 0; i < 8; i++) {
        wire[i] = be[7 - i];
    }
    return true;
}

static const char *find(const zigbee_zloe_txt_t *items, size_t count, const char *key)
{
    for (size_t i = 0; i < count; i++) {
        if (items[i].key != NULL && strcmp(items[i].key, key) == 0) {
            return items[i].value;
        }
    }
    return NULL;
}

void zigbee_zloe_txt_build(const zigbee_zloe_record_t *record, zigbee_zloe_txt_t *items, zigbee_zloe_txt_buf_t *buf)
{
    hex_of(record->eui64, buf->xa);
    hex_of(record->epan, buf->xp);
    snprintf(buf->nwk, sizeof(buf->nwk), "%04x", record->nwk);
    items[0] = (zigbee_zloe_txt_t) {
        "v", "2"
    };
    items[1] = (zigbee_zloe_txt_t) {
        "xa", buf->xa
    };
    items[2] = (zigbee_zloe_txt_t) {
        "xp", buf->xp
    };
    items[3] = (zigbee_zloe_txt_t) {
        "nwk", buf->nwk
    };
    items[4] = (zigbee_zloe_txt_t) {
        "role", "router"
    };
    items[5] = (zigbee_zloe_txt_t) {
        "sec", "none"
    };
}

bool zigbee_zloe_txt_parse(const zigbee_zloe_txt_t *items, size_t count, zigbee_zloe_record_t *out)
{
    const char *sec = find(items, count, "sec");
    const char *role = find(items, count, "role");
    uint8_t nwk[2];

    if (sec == NULL || strcmp(sec, "none") != 0 || !parse_eui64(find(items, count, "xa"), out->eui64) ||
            !parse_eui64(find(items, count, "xp"), out->epan) || !parse_hex(find(items, count, "nwk"), 4, nwk)) {
        return false;
    }
    out->nwk = (uint16_t)(nwk[0] << 8 | nwk[1]);
    out->coordinator = role != NULL && strcmp(role, "coordinator") == 0;
    return true;
}

size_t zigbee_zloe_keep_link_local(uint8_t (*addrs)[16], size_t count)
{
    size_t kept = 0;

    for (size_t i = 0; i < count; i++) {
        if (addrs[i][0] == 0xFE && (addrs[i][1] & 0xC0) == 0x80) {
            memmove(addrs[kept], addrs[i], 16);
            kept++;
        }
    }
    return kept;
}
