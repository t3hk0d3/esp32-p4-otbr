#include <stdio.h>
#include <string.h>
#include "device_api_priv.h"

void json_out_init(json_out_t *w, char *buf, size_t cap)
{
    w->buf = buf;
    w->cap = cap;
    w->len = 0;
    w->overflow = false;
    if (cap > 0) {
        buf[0] = '\0';
    }
}

static void put(json_out_t *w, const char *text, size_t n)
{
    if (w->cap == 0) {
        w->overflow = true;
        return;
    }
    const size_t room = w->cap - 1 - w->len;
    const size_t take = n < room ? n : room;

    memcpy(w->buf + w->len, text, take);
    w->len += take;
    w->buf[w->len] = '\0';
    if (take < n) {
        w->overflow = true;
    }
}

void json_out_raw(json_out_t *w, const char *text)
{
    put(w, text, strlen(text));
}

// Length of the well-formed UTF-8 sequence at `p` (lead byte >= 0x80), 0 if it is not one. Overlongs and surrogates rejected.
static size_t utf8_len(const char *p)
{
    const unsigned char *u = (const unsigned char *)p;
    size_t n;

    if (u[0] >= 0xc2 && u[0] <= 0xdf) {
        n = 2;
    } else if (u[0] >= 0xe0 && u[0] <= 0xef) {
        n = 3;
    } else if (u[0] >= 0xf0 && u[0] <= 0xf4) {
        n = 4;
    } else {
        return 0;
    }
    for (size_t i = 1; i < n; i++) {
        if ((u[i] & 0xc0) != 0x80) { // also stops at the NUL terminator
            return 0;
        }
    }
    if ((u[0] == 0xe0 && u[1] < 0xa0) || (u[0] == 0xed && u[1] > 0x9f) || (u[0] == 0xf0 && u[1] < 0x90) ||
            (u[0] == 0xf4 && u[1] > 0x8f)) {
        return 0;
    }
    return n;
}

void json_out_str(json_out_t *w, const char *text)
{
    if (text == NULL) {
        json_out_raw(w, "null");
        return;
    }
    put(w, "\"", 1);
    for (const char *p = text; *p; p++) {
        const unsigned char c = (unsigned char) * p;

        switch (c) {
        case '"':
            put(w, "\\\"", 2);
            break;
        case '\\':
            put(w, "\\\\", 2);
            break;
        case '\n':
            put(w, "\\n", 2);
            break;
        case '\r':
            put(w, "\\r", 2);
            break;
        case '\t':
            put(w, "\\t", 2);
            break;
        default:
            if (c < 0x20) {
                char esc[8];
                snprintf(esc, sizeof(esc), "\\u%04x", c);
                put(w, esc, 6);
            } else if (c < 0x80) {
                put(w, p, 1);
            } else {
                const size_t n = utf8_len(p);
                if (n == 0) {
                    put(w, "?", 1); // not valid UTF-8: JSON must stay valid
                } else {
                    put(w, p, n);
                    p += n - 1;
                }
            }
        }
    }
    put(w, "\"", 1);
}

void json_out_uint(json_out_t *w, uint32_t value)
{
    char tmp[16];
    put(w, tmp, (size_t)snprintf(tmp, sizeof(tmp), "%lu", (unsigned long)value));
}

void json_out_u64(json_out_t *w, uint64_t value)
{
    char tmp[24];
    put(w, tmp, (size_t)snprintf(tmp, sizeof(tmp), "%llu", (unsigned long long)value));
}

void json_out_int(json_out_t *w, int32_t value)
{
    char tmp[16];
    put(w, tmp, (size_t)snprintf(tmp, sizeof(tmp), "%ld", (long)value));
}

void json_out_bool(json_out_t *w, bool value)
{
    json_out_raw(w, value ? "true" : "false");
}
