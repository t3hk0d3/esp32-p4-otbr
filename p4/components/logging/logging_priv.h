#pragma once

// Internals of the logging component shared between its sources, plus the ops seam the host tests use to observe
// NVS and esp_log_level_set.

#include <stddef.h>
#include "esp_err.h"
#include "esp_log_level.h"
#include "logging.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LOGGING_NVS_NAMESPACE "logging"
#define LOGGING_NVS_KEY "settings"
#define LOGGING_BLOB_VERSION 1
/** Stored blob: the version byte, then logging_settings_t as laid out in memory. */
#define LOGGING_BLOB_SIZE (1 + sizeof(logging_settings_t))
#define LOGGING_LISTENERS_MAX 4

/** NVS and log-level seam: the target defaults are the real functions, host tests set fakes. */
typedef struct {
    /**
     * Read blob `ns`/`key` into `buf`. `*len` holds the capacity on entry and the blob size on ESP_OK.
     * ESP_ERR_NOT_FOUND: no such blob; ESP_ERR_INVALID_SIZE: blob larger than the capacity.
     */
    esp_err_t (*blob_get)(const char *ns, const char *key, void *buf, size_t *len);
    /** Write and commit blob `ns`/`key`. */
    esp_err_t (*blob_set)(const char *ns, const char *key, const void *buf, size_t len);
    void (*level_set)(const char *tag, esp_log_level_t level);
} logging_ops_t;

/** Replace the ops (host tests). Call before logging_init. */
void logging_set_ops(const logging_ops_t *ops);

/** Back to the uninitialised state: settings, listeners and viewers_max forgotten (host tests). */
void logging_reset(void);

/**
 * The highest level any live viewer wants changed (ESP_LOG_NONE when there is none): recompute and set every gate,
 * then call the listeners whose effective level changed, on the calling task. No-op before logging_init.
 */
void logging_viewers_max_changed(esp_log_level_t viewers_max);

#ifdef __cplusplus
}
#endif
