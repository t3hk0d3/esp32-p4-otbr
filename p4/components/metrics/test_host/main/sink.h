#pragma once

// Test helpers: a flush target that collects everything, and a full sample input.

#include <stddef.h>

#include "esp_err.h"
#include "metrics_types.h"

typedef struct {
    char *data;
    size_t len;
    size_t flushes;
    size_t fail_at; /**< flush number that fails with ESP_FAIL, 0 = never */
} sink_t;

esp_err_t sink_flush(void *ctx, const char *data, size_t len);
void sink_free(sink_t *s);

typedef struct {
    metrics_device_t device;
    diag_metrics_view_t diag;
    metrics_collector_view_t collector;
    metrics_input_t in;
} sample_input_t;

/** Every source available, a fresh valid cache, every field non-zero where it can be. */
void sample_input(sample_input_t *s);

/** Render `in` through a writer with a `cap`-byte buffer into a NUL-terminated heap string. */
char *render_to_string(const metrics_input_t *in, size_t cap);
