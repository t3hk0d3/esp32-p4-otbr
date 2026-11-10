#pragma once

// Text helpers of the logging component. Host-tested in test_host/.

#include <stddef.h>
#include <stdint.h>
#include <sys/time.h>
#include "esp_err.h"
#include "esp_log_level.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Split a syslog target `host[:port]` into a host (IPv6 without its brackets) and a port (514 when omitted).
 * Accepts IPv4, hostname, `[IPv6]` and `[IPv6]:port`. ESP_ERR_INVALID_ARG for an empty host, a port outside 1..65535,
 * an unbracketed IPv6 address, or a host that does not fit `host_cap`; the outputs are then untouched.
 */
esp_err_t logging_target_parse(const char *in, char *host, size_t host_cap, uint16_t *port);

/**
 * Find the message inside a line written by ESP-IDF's logger: strips a leading ANSI colour plus `X (<digits>) <tag>: `
 * and a trailing ANSI reset, `\r` and `\n`. Text without that prefix keeps its start. `*body` points into `text`.
 * Returns the body length, clamped to LOGGING_LINE_MAX + 1 so the caller can tell that the line is too long; copy
 * at most LOGGING_LINE_MAX bytes and pass them through logging_text_truncate().
 */
size_t logging_text_body(const char *text, size_t len, const char **body);

/**
 * Fit a body into `cap` bytes. `buf` holds `len` bytes (len above cap means the source was longer); when len > cap the
 * end is replaced by the marker `…[cut]` (UTF-8), cutting at a character boundary. Returns the final length (<= cap).
 */
size_t logging_text_truncate(char *buf, size_t len, size_t cap);

/**
 * Encode one RFC 5424 message: facility local0, severity from `level`, TIMESTAMP from `wall` (UTC, milliseconds; NULL =
 * NILVALUE), `hostname` and `tag` (APP-NAME) sanitized to printable ASCII, MSG = `<uptime_ms> <msg>`. Returns the
 * length written, or -1 when `out` is too small (then `out` is NUL-terminated and holds a prefix).
 */
int logging_syslog_format(char *out, size_t cap, esp_log_level_t level, const char *hostname, const char *tag,
                          uint32_t uptime_ms, const struct timeval *wall, const char *msg, size_t msg_len);

#ifdef __cplusplus
}
#endif
