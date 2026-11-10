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

/** Called with a tag's new effective level (what esp_log_level_set got for it). */
typedef void (*logging_level_cb_t)(esp_log_level_t effective);

/**
 * Load the settings from NVS (defaults when there are none, or a warning and defaults when the stored ones are of
 * another version, size or invalid) and set the ESP-IDF log levels from them. Call once, early in app_main.
 */
esp_err_t logging_init(void);

/** A copy of the settings in force. ESP_ERR_INVALID_STATE before logging_init. */
esp_err_t logging_get_settings(logging_settings_t *out);

/**
 * Validate, put in force and store a whole settings struct; concurrent calls are serialised (the last one wins whole).
 * Listeners whose tag's effective level changed are then called on this task, after the internal lock is released.
 * ESP_ERR_INVALID_ARG: invalid, nothing changed; `*bad_field` (may be NULL) names the field, valid until the next
 * failed logging_apply. ESP_FAIL: the NVS write failed, the settings are in force anyway (until reboot).
 */
esp_err_t logging_apply(const logging_settings_t *settings, const char **bad_field);

/**
 * Call `cb` whenever the effective level of `tag` changes (up to 4 listeners). Not called at registration: the
 * baseline is the level in force then. ESP_ERR_NO_MEM when full, ESP_ERR_INVALID_STATE before logging_init.
 */
esp_err_t logging_level_listener_add(const char *tag, logging_level_cb_t cb);

#ifdef __cplusplus
}
#endif
