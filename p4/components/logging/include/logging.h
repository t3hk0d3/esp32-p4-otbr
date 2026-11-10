#pragma once

// Logging: per-channel (UART, USB-JTAG, syslog, live view) and per-component log levels. This header holds the data
// shapes of the public contract; the pure decisions live in logging_core.h.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/time.h>
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
#define LOGGING_VIEWERS_MAX 2
#define LOGGING_SYSLOG_PORT 514
/** lwIP sockets the component holds at most: the syslog UDP socket (counted in routing/task.c SOCKETS_NEEDED). */
#define LOGGING_SOCKETS 1

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
    LOGGING_SYSLOG_NO_SOCKET, /**< Resolved, but no UDP socket could be made (lwIP out of sockets or memory). */
} logging_syslog_state_t;

typedef struct {
    bool jtag_host;
    logging_syslog_state_t syslog;
    uint8_t viewers;
    uint32_t sent[LOGGING_CH_COUNT];
    uint32_t dropped[LOGGING_CH_COUNT];
    uint32_t live_sent;    /**< Lines queued for at least one viewer. */
    uint32_t live_dropped; /**< Lines a viewer wanted that the full ring refused or a full viewer queue dropped. */
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

/**
 * The highest level this build compiles in (CONFIG_LOG_MAXIMUM_LEVEL): ESP_LOG_INFO in a release build, ESP_LOG_VERBOSE
 * in a test build. logging_apply refuses levels above it; stored ones above it are lowered to it at logging_init; live
 * viewers cannot open or move above it.
 */
esp_log_level_t logging_level_ceiling(void);

/** A copy of the settings in force. ESP_ERR_INVALID_STATE before logging_init. */
esp_err_t logging_get_settings(logging_settings_t *out);

/**
 * Validate, put in force and store a whole settings struct; concurrent calls are serialised (the last one wins whole).
 * Listeners whose tag's effective level changed are then called on this task, after the internal lock is released.
 * ESP_ERR_INVALID_ARG: invalid or a level above logging_level_ceiling(), nothing changed; `*bad_field` (may be NULL) names the field, valid until the next
 * failed logging_apply. ESP_FAIL: the NVS write failed, the settings are in force anyway (until reboot).
 */
esp_err_t logging_apply(const logging_settings_t *settings, const char **bad_field);

/**
 * Call `cb` once now, on the calling task, with the effective level of `tag`, then whenever that level changes (up
 * to 4 listeners). Callbacks run outside the settings lock and may call logging_get_settings, but not logging_apply
 * or logging_level_listener_add. ESP_ERR_NO_MEM when full, ESP_ERR_INVALID_STATE before logging_init.
 */
esp_err_t logging_level_listener_add(const char *tag, logging_level_cb_t cb);

/**
 * Start the capture: the transit ring, the USB-Serial-JTAG driver and the drain task "log_out" (MGMT_CORE), which
 * writes captured lines to USB-JTAG and syslog. Lines logged before this go to the UART only. Call after logging_init
 * and esp_netif_init. ESP_ERR_INVALID_STATE before logging_init, ESP_ERR_NO_MEM when the ring or the task cannot be
 * made (the UART keeps working either way); a JTAG driver that fails to install only leaves USB-JTAG silent.
 */
esp_err_t logging_start(void);

/**
 * Mark the calling task as a log sink until the matching logging_sink_leave (nestable, per task): lines it logs in
 * between reach the UART only, never the ring, so sending a line cannot produce lines that are sent again. Up to 8
 * tasks at once; beyond that the call is ignored (the sink's lines then loop back, so keep the sinks few).
 */
void logging_sink_enter(void);
void logging_sink_leave(void);

/** Counters and sink state since boot; zeroes before logging_start. */
void logging_get_status(logging_status_t *out);

/**
 * Wall clock for syslog TIMESTAMP: `now` fills `out` and returns true while the clock is valid. NULL (the default) =
 * NILVALUE. Called on the drain task.
 */
void logging_set_clock(bool (*now)(struct timeval *out));

/**
 * Tags of the lines the drain has seen since boot (at most LOGGING_KNOWN_TAGS_MAX), in first-seen order: up to `cap`
 * pointers into a set that only grows, valid for the board's lifetime. Returns how many.
 */
size_t logging_known_tags(const char **out, size_t cap);

/** A live viewer's queue: lines wait here (PSRAM) until the viewer reads them; a full queue drops new lines. */
#define LOGGING_VIEWER_QUEUE (8 * 1024)

/** A live log viewer (a WebSocket client): its own level and queue. */
typedef struct logging_viewer logging_viewer_t;

/**
 * Open a viewer at `level` (ESP_LOG_NONE..logging_level_ceiling()): from now on every captured line at or below it (and its
 * component's level) is queued for it, and `wake(ctx)` (may be NULL) is called after each line queued. `wake` runs on
 * the drain task with the viewers' lock held: it must not block, log, touch lwIP or call a logging_viewer_ function.
 * The generation gate is recomputed (the highest open viewer level counts as a channel). The queue is
 * LOGGING_VIEWER_QUEUE bytes of PSRAM, allocated here. NULL when LOGGING_VIEWERS_MAX viewers are open, the level is out
 * of range or there is no memory.
 */
logging_viewer_t *logging_viewer_open(esp_log_level_t level, void (*wake)(void *ctx), void *ctx);

/** Change a viewer's level (ignored outside ESP_LOG_NONE..logging_level_ceiling()); the gate is recomputed. */
void logging_viewer_set_level(logging_viewer_t *viewer, esp_log_level_t level);

/**
 * Take the viewer's oldest queued line. `*dropped_since` (may be NULL) gets the lines its full queue dropped since the
 * last call that reported drops (0 when none), whether or not a line is returned. False when the queue is empty.
 * `out->text` stays valid until the next read or the close. One reader per viewer; read and close from the same task.
 */
bool logging_viewer_read(logging_viewer_t *viewer, logging_line_t *out, uint32_t *dropped_since);

/** Close a viewer (NULL is ignored): no more lines, the queue is freed, the gate is recomputed. */
void logging_viewer_close(logging_viewer_t *viewer);

/** Register the console command `log` (show and set the settings from the UART). Call before esp_console_start_repl. */
esp_err_t logging_console_register(void);

#ifdef __cplusplus
}
#endif
