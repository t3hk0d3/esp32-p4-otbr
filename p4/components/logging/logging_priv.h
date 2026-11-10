#pragma once

// Internals of the logging component shared between its sources, plus the ops seam the host tests use to observe
// NVS and esp_log_level_set.

#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>
#include "esp_err.h"
#include "esp_log_level.h"
#include "logging.h"
#include "logging_core.h"
#include "logging_ring.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

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

/**
 * The channel levels and the viewers' level in force, read without a lock (one atomic load): what the capture path
 * uses, because it must never take the settings mutex (lines are logged while it is held). All off before
 * logging_init.
 */
logging_levels_t logging_levels(void);

/** Goes up by one on every successful logging_apply: the drain task re-reads the settings when it changes. */
uint32_t logging_settings_generation(void);

/** An unresolved syslog target is looked up again this long after the last try. */
#define LOGGING_SYSLOG_RETRY_MS 60000

/** Sink seam of the drain: the target defaults use the USB-Serial-JTAG driver, lwIP and netnames. */
typedef struct {
    bool (*jtag_connected)(void);
    /** Queue `len` bytes for USB-JTAG without waiting: returns `len`, or 0 when it took nothing (never a part). */
    int (*jtag_write)(const void *buf, size_t len);
    /** Look `host` up (may block); `addr` gets its address with `port`. */
    esp_err_t (*resolve)(const char *host, uint16_t port, struct sockaddr_storage *addr, socklen_t *addr_len);
    /** A UDP socket of `family`, or -1. */
    int (*udp_open)(int family);
    /** Send one datagram without blocking; -1 on error. */
    int (*udp_send)(int fd, const void *buf, size_t len, const struct sockaddr *to, socklen_t to_len);
    void (*udp_close)(int fd);
    /** HOSTNAME of the syslog lines. */
    const char *(*hostname)(void);
} logging_drain_ops_t;

/** Replace the sink ops (host tests). */
void logging_drain_set_ops(const logging_drain_ops_t *ops);

/** Sinks back to their boot state: syslog off without a socket, counters and known tags cleared (host tests). */
void logging_drain_reset(void);

/**
 * Syslog settings in force: off (level ESP_LOG_NONE or no target) closes the socket; a new target closes it and
 * resolves the target at the next logging_drain_tick; the same target keeps the socket. Drain task only.
 */
void logging_drain_configure(esp_log_level_t syslog_level, const char *target);

/** Resolve the syslog target when due (blocks while it looks it up) and open the socket. Drain task only. */
void logging_drain_tick(uint32_t now_ms);

/** Write one record to each sink whose level it passes in `lv`, counting it sent or dropped. Drain task only. */
void logging_drain_line(const logging_line_t *line, logging_levels_t lv);

/** The sinks' part of logging_status_t: jtag_host, syslog, sent and dropped of JTAG and syslog. Any task. */
void logging_drain_status(logging_status_t *out);

/**
 * Target only: install the USB-Serial-JTAG driver (a failure only leaves USB-JTAG silent) and start the drain task
 * "log_out" on MGMT_CORE, which pops `ring` when notified. ESP_ERR_NO_MEM when the task cannot be made.
 */
esp_err_t logging_drain_start(logging_ring_t *ring, TaskHandle_t *task);

/** Whether logging_init has run (the settings are in force). */
bool logging_is_initialised(void);

#ifdef __cplusplus
}
#endif
