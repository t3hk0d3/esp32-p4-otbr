#include "writer.h"

#include <string.h>

void metrics_writer_init(metrics_writer_t *w, char *buf, size_t cap, metrics_flush_fn flush, void *ctx)
{
    *w = (metrics_writer_t) {
        .buf = buf, .cap = cap, .flush = flush, .ctx = ctx, .err = ESP_OK
    };
}

static void flush_buffer(metrics_writer_t *w)
{
    if (w->err == ESP_OK && w->len > 0) {
        w->err = w->flush(w->ctx, w->buf, w->len);
    }
    w->len = 0;
}

static void put(metrics_writer_t *w, const char *text, size_t n)
{
    while (n > 0 && w->err == ESP_OK) {
        if (w->len == w->cap) {
            flush_buffer(w);
            continue;
        }
        const size_t room = w->cap - w->len;
        const size_t take = n < room ? n : room;

        memcpy(w->buf + w->len, text, take);
        w->len += take;
        text += take;
        n -= take;
    }
}

void metrics_writer_raw(metrics_writer_t *w, const char *text)
{
    put(w, text, strlen(text));
}

void metrics_writer_u64(metrics_writer_t *w, uint64_t value)
{
    char digits[20];
    size_t n = 0;

    do {
        digits[sizeof(digits) - 1 - n++] = (char)('0' + value % 10);
        value /= 10;
    } while (value > 0);
    put(w, digits + sizeof(digits) - n, n);
}

void metrics_writer_usec(metrics_writer_t *w, uint64_t usec)
{
    char frac[7];
    uint32_t rest = (uint32_t)(usec % 1000000);

    metrics_writer_u64(w, usec / 1000000);
    frac[0] = '.';
    for (int i = 6; i >= 1; i--) {
        frac[i] = (char)('0' + rest % 10);
        rest /= 10;
    }
    put(w, frac, sizeof(frac));
}

void metrics_writer_tenths(metrics_writer_t *w, int32_t tenths)
{
    const int64_t wide = tenths;
    const uint64_t magnitude = (uint64_t)(wide < 0 ? -wide : wide);

    if (tenths < 0) {
        put(w, "-", 1);
    }
    metrics_writer_u64(w, magnitude / 10);
    put(w, ".", 1);
    metrics_writer_u64(w, magnitude % 10);
}

void metrics_writer_label_value(metrics_writer_t *w, const char *text)
{
    while (*text) {
        const size_t run = strcspn(text, "\\\"\n"); // plain bytes up to the next character to escape

        put(w, text, run);
        text += run;
        if (*text) {
            put(w, *text == '\n' ? "\\n" : *text == '"' ? "\\\"" : "\\\\", 2);
            text++;
        }
    }
}

esp_err_t metrics_writer_finish(metrics_writer_t *w)
{
    flush_buffer(w);
    return w->err;
}
