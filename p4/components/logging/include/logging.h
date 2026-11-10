#pragma once

// Logging: per-channel (UART, USB-JTAG, syslog, live view) and per-component log levels. This header holds the data
// shapes of the public contract; the pure decisions live in logging_core.h.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_log_level.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LOGGING_TAG_MAX 24 /**< Tag buffer size, NUL included. */
#define LOGGING_COMPONENTS_MAX 32
#define LOGGING_KNOWN_TAGS_MAX 64
#define LOGGING_TARGET_MAX 128
#define LOGGING_LINE_MAX 512 /**< Body bytes, truncation marker included. */
#define LOGGING_VIEWERS_MAX 4
#define LOGGING_SYSLOG_PORT 514

typedef enum {
    LOGGING_CH_UART,
    LOGGING_CH_JTAG,
    LOGGING_CH_SYSLOG,
    LOGGING_CH_COUNT,
} logging_channel_t;

typedef struct {
    char tag[LOGGING_TAG_MAX];
    esp_log_level_t level;
} logging_component_t;

typedef struct {
    esp_log_level_t channel[LOGGING_CH_COUNT];
    char syslog_target[LOGGING_TARGET_MAX]; /**< host[:port]; empty = none. */
    esp_log_level_t default_level;          /**< The `*` component. */
    uint8_t n_components;
    logging_component_t components[LOGGING_COMPONENTS_MAX];
} logging_settings_t;

typedef enum {
    LOGGING_SYSLOG_OFF,
    LOGGING_SYSLOG_RESOLVING,
    LOGGING_SYSLOG_SENDING,
    LOGGING_SYSLOG_UNRESOLVED,
} logging_syslog_state_t;

typedef struct {
    bool jtag_host;
    logging_syslog_state_t syslog;
    uint8_t viewers;
    uint32_t sent[LOGGING_CH_COUNT];
    uint32_t dropped[LOGGING_CH_COUNT];
    uint32_t live_sent;    /**< Lines queued to any viewer. */
    uint32_t live_dropped;
} logging_status_t;

typedef struct {
    uint32_t uptime_ms;
    esp_log_level_t level;
    char tag[LOGGING_TAG_MAX];
    uint16_t len;
    const char *text; /**< Not NUL-terminated; valid until the next read on that viewer. */
} logging_line_t;

#ifdef __cplusplus
}
#endif
