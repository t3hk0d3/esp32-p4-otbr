#pragma once

// Text writer for the exposition format: fills a caller buffer and hands every full buffer to a flush callback, so a
// body of any size is sent in chunks without being held in memory. Pure: host-tested in test_host/.

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "metrics.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char *buf;
    size_t cap;
    size_t len;
    metrics_flush_fn flush;
    void *ctx;
    esp_err_t err; /**< first flush error; once set, nothing more is written */
} metrics_writer_t;

void metrics_writer_init(metrics_writer_t *w, char *buf, size_t cap, metrics_flush_fn flush, void *ctx);
void metrics_writer_raw(metrics_writer_t *w, const char *text);
void metrics_writer_u64(metrics_writer_t *w, uint64_t value);
/** Microseconds as seconds with six decimals: 1500000 -> "1.500000". */
void metrics_writer_usec(metrics_writer_t *w, uint64_t usec);
/** Tenths of a unit as a signed decimal with one fraction digit: 473 -> "47.3", -5 -> "-0.5". */
void metrics_writer_tenths(metrics_writer_t *w, int32_t tenths);
/** A label value with \ " and newline escaped as \\ \" \n. */
void metrics_writer_label_value(metrics_writer_t *w, const char *text);
/** Flush what is buffered. Returns the first flush error, or ESP_OK. */
esp_err_t metrics_writer_finish(metrics_writer_t *w);

#ifdef __cplusplus
}
#endif
