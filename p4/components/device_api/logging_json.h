#pragma once

// /device/logging bodies and the live log WebSocket's text (logging_json.c). Pure: host-tested in test_host/.

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "logging.h"

#ifdef __cplusplus
extern "C" {
#endif

/** PUT /device/logging: bodies above this many bytes are refused with 413. */
#define DEVICE_API_LOGGING_BODY_MAX 4096
/** Room for the GET body at its largest: every override and known tag, escaped. */
#define DEVICE_API_LOGGING_JSON_MAX 8192
/**
 * Longest text frame the live log WebSocket sends: "<L> (<uptime>) <tag>: <body>" of the longest line with every byte
 * of its tag and body ill-formed UTF-8 (each then the 3 bytes of U+FFFD).
 */
#define DEVICE_API_LOGS_FRAME_MAX (3 * (LOGGING_LINE_MAX + LOGGING_TAG_MAX) + sizeof("X (4294967295) : "))

/**
 * The GET and PUT /device/logging body:
 * {"max_level":L,"channels":{"uart":{"level":L},"jtag":{"level":L,"host":bool},"syslog":{"level":L,"target":"host[:port]","state":
 * "off|resolving|sending|unresolved"}},"components":{"*":L,"<tag>":L,...},"known_tags":["<tag>",...],"viewers":n,
 * "counters":{"uart":{"sent":n,"dropped":n},"jtag":{...},"syslog":{...},"live":{"sent":n,"dropped":n}}}.
 * max_level is the highest level this build compiles in (logging_level_ceiling()). Returns its length, or -1 when `cap`
 * is too small.
 */
int device_api_logging_json(char *out, size_t cap, const logging_settings_t *settings, const logging_status_t *status,
                            const char **known, size_t n_known);

/**
 * Apply a PUT /device/logging body over `inout`: any subset of channels.<uart|jtag|syslog>.level,
 * channels.syslog.target and components. `components` replaces the override set; its "*" key sets default_level (left
 * as it is when absent); a tag (or "*") listed twice is refused. Fields GET returns but PUT does not take (host,
 * state, known_tags, viewers, counters, max_level) and unknown keys, at the top level or in a channel, are ignored, so a client
 * can send back what GET gave it. The target's syntax is checked here; the rest of the rules (a syslog level above off
 * needs a target) by logging_apply. ESP_ERR_INVALID_ARG: `*bad_field` names the field ("body", "channels",
 * "channels.<name>", "channels.<name>.level", "channels.syslog.target", "components", "components.<tag>"), valid until
 * the next call; `inout` is then untouched. ESP_ERR_NO_MEM: no memory.
 */
esp_err_t device_api_logging_parse(const char *body, logging_settings_t *inout, const char **bad_field);

/**
 * The live log's frame for a line: "<L> (<uptime>) <tag>: <body>", L one of E W I D V, valid UTF-8 (each ill-formed
 * subpart of the tag or body replaced by U+FFFD). Length, or -1 when `cap` is too small.
 */
int device_api_logs_line(char *out, size_t cap, const logging_line_t *line);

/** The live log's notice of lines dropped for this client: "# dropped <n> lines". Length, or -1. */
int device_api_logs_dropped(char *out, size_t cap, uint32_t n);

/**
 * The client's level from the WebSocket URL's query: `level=<off|error|warning|info|debug|verbose>`, info when there is
 * no query or no level. ESP_ERR_INVALID_ARG for any other value or a level above logging_level_ceiling() (`*out`
 * untouched).
 */
esp_err_t device_api_logs_query_level(const char *query, esp_log_level_t *out);

/**
 * A text frame from the client: {"level":"<name>"} (`len` bytes, not NUL-terminated). ESP_ERR_INVALID_ARG for anything
 * else or a level above logging_level_ceiling() (`*out` untouched).
 */
esp_err_t device_api_logs_level_frame(const char *frame, size_t len, esp_log_level_t *out);

/** Refusal of a new live log client while `open_viewers` are connected: 503 at LOGGING_VIEWERS_MAX, else 0. */
int device_api_logs_ws_refusal(size_t open_viewers);

/** Detail of that 503. */
#define DEVICE_API_LOGS_TOO_MANY "too many viewers"

#ifdef __cplusplus
}
#endif
