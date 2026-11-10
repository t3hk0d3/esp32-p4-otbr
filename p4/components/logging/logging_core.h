#pragma once

// Pure decisions of the logging component: level names, the settings model, the generation gate, validation and the
// set of tags seen since boot. Host-tested in test_host/.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "logging.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Name of a level: off, error, warning, info, debug, verbose; "?" outside 0..5. Static string. */
const char *logging_level_name(esp_log_level_t level);

/** Level of a name (exact, lower case). ESP_ERR_INVALID_ARG for anything else; `out` is then untouched. */
esp_err_t logging_level_parse(const char *name, esp_log_level_t *out);

/** UART info, JTAG info, syslog off, no target, default info, no overrides. */
void logging_core_defaults(logging_settings_t *s);

/** The component's override, else default_level. */
esp_log_level_t logging_core_tag_level(const logging_settings_t *s, const char *tag);

/** Highest level any channel wants: the three channel levels and `viewers_max`. */
esp_log_level_t logging_core_channels_max(const logging_settings_t *s, esp_log_level_t viewers_max);

/** Level to hand to esp_log_level_set for `tag`: the component's level capped by what any channel wants. */
esp_log_level_t logging_core_gate(const logging_settings_t *s, const char *tag, esp_log_level_t viewers_max);

/** Whether a line passes its component's level and the channel's level. */
bool logging_core_passes(esp_log_level_t line, esp_log_level_t component, esp_log_level_t channel);

/**
 * Add or replace an override. INVALID_ARG: bad tag (empty, "*", 24+ bytes, or anything but printable ASCII without
 * spaces, quotes and backslashes) or level; NO_MEM: set is full.
 */
esp_err_t logging_core_set_component(logging_settings_t *s, const char *tag, esp_log_level_t level);

/** Remove an override. NOT_FOUND when there is none. */
esp_err_t logging_core_remove_component(logging_settings_t *s, const char *tag);

/**
 * Check a whole settings struct, including what logging_core_set_component would refuse: a component named "*" and a
 * tag listed twice. On INVALID_ARG `*field` (may be NULL) names the culprit: "channels.<name>.level",
 * "channels.syslog.target", "components.*" or "components.<tag>". The string is static and rewritten by the next
 * call, so copy it at once; callers serialise validation.
 */
esp_err_t logging_core_validate(const logging_settings_t *s, const char **field);

/**
 * Whether any level in `s` is above `ceiling` (the highest level the build compiles in, logging_level_ceiling()).
 * INVALID_ARG names the first one in `*field` (may be NULL), as logging_core_validate does: "channels.<name>.level",
 * "components.*" or "components.<tag>"; the string is static and rewritten by the next call.
 */
esp_err_t logging_core_above(const logging_settings_t *s, esp_log_level_t ceiling, const char **field);

/** Lower every level in `s` above `ceiling` to `ceiling`. True when one was lowered. */
bool logging_core_clamp(logging_settings_t *s, esp_log_level_t ceiling);

/**
 * Channel levels the capture and the drain read without a lock: one 32-bit word, published atomically by the settings
 * code on every change (logging_levels() in logging_priv.h).
 */
typedef union {
    struct {
        uint8_t channel[LOGGING_CH_COUNT]; /**< esp_log_level_t of each channel. */
        uint8_t viewers;                   /**< Highest level a live viewer wants; ESP_LOG_NONE without viewers. */
    };
    uint32_t word;
} logging_levels_t;

_Static_assert(sizeof(logging_levels_t) == sizeof(uint32_t), "logging_levels_t must load and store as one word");

/** The channel levels of `s` and `viewers_max`, packed. */
logging_levels_t logging_core_levels(const logging_settings_t *s, esp_log_level_t viewers_max);

/**
 * Whether `tag` belongs to the network path (prefixes httpd, esp.emac, esp_eth, esp_netif, lwip, vfs_calls): ESP-IDF's
 * web server, Ethernet driver, esp_netif, lwIP and the VFS select() the web server's loop waits in, which log while a
 * live frame or a syslog datagram goes out or its ACK or ICMP answer comes in. NULL: false.
 */
bool logging_core_net_path_tag(const char *tag);

/**
 * Highest level syslog or a live viewer at `level` gets of `tag`: `level`, except for the network-path tags
 * (logging_core_net_path_tag), which are capped at info. Their debug lines, sent over the network, would make more
 * frames and so more lines, without end (Rulings R13, R14). The UART and USB-JTAG are not capped: they do not use the
 * network.
 */
esp_log_level_t logging_core_net_cap(const char *tag, esp_log_level_t level);

/**
 * Highest level a channel other than the UART wants of `tag`: JTAG, and syslog and the viewers capped by
 * logging_core_net_cap. NULL `tag`: uncapped.
 */
esp_log_level_t logging_core_nonuart_max(logging_levels_t lv, const char *tag);

/**
 * `lv` with the JTAG level off while no USB host is there (`host` false): the capture then formats no line for
 * USB-JTAG alone, and the drain counts nothing against it.
 */
logging_levels_t logging_core_jtag_gate(logging_levels_t lv, bool host);

#define LOGGING_ROUTE_UART (1u << 0) /**< Hand the line to ESP-IDF's writer (the UART), which applies the tag level. */
#define LOGGING_ROUTE_RING (1u << 1) /**< Format the line into the transit ring for the other channels. */

/**
 * Where a captured log line goes. `in_isr` (a constrained context: ISR, cache off, scheduler not running; or before
 * logging_start) hands every line to ESP-IDF unchanged; the target wrapper makes that test itself, in IRAM.
 * Otherwise UART when `level` is at or below `uart_level`; ring when no sink guard is held, `level` is at or below
 * `nonuart_max` and the tag's ESP-IDF level `tag_level` (the gate: the component level capped by the channels) lets
 * it through. A line at ESP_LOG_NONE goes nowhere. Returns LOGGING_ROUTE_* bits.
 */
unsigned logging_capture_route(esp_log_level_t level, esp_log_level_t uart_level, esp_log_level_t nonuart_max,
                               esp_log_level_t tag_level, bool in_guard, bool in_isr);

#define LOGGING_WANT_LIVE (1u << LOGGING_CH_COUNT) /**< logging_core_wanting: the live viewers. */

/**
 * The channels other than the UART that want a line of `level` from `tag`: bit (1 << channel), plus LOGGING_WANT_LIVE
 * for the viewers; syslog and the viewers capped by logging_core_net_cap. Counts a line the full ring refused against
 * each of them.
 */
unsigned logging_core_wanting(esp_log_level_t level, const char *tag, logging_levels_t lv);

/**
 * Parse the arguments of the console command `log` (argv[0] is "log") and apply them to `inout`:
 *   log level <uart|jtag|syslog> <level>      set a channel level
 *   log tag <tag|*> <level|default>           set an override; `*` sets the default level, `default` removes an override
 *   log syslog <host[:port]|off>              set the target (the level stays as it is); `off` clears it and the level
 * `argc == 1` (plain `log`, the display) is not an assignment and is refused too. On error `*err` (may be NULL) points
 * to a static message, `inout` is untouched, and the result is ESP_ERR_INVALID_ARG, or ESP_ERR_NO_MEM when the
 * override set is full. The result is not validated as a whole (logging_apply does that).
 */
esp_err_t logging_console_parse(int argc, char **argv, logging_settings_t *inout, const char **err);

/** Tags seen since boot, in insertion order. */
typedef struct {
    size_t count;
    char tags[LOGGING_KNOWN_TAGS_MAX][LOGGING_TAG_MAX];
} logging_tagset_t;

/**
 * Remember a tag. True only when it was newly added; false when known, invalid (as for logging_core_set_component) or
 * the set is full.
 * One writer at a time; the new entry is written before `count` publishes it (release), so readers need no lock.
 */
bool logging_tagset_add(logging_tagset_t *set, const char *tag);

/** Copy up to `cap` pointers (into the set) in insertion order; returns how many. Safe beside a concurrent add. */
size_t logging_tagset_list(const logging_tagset_t *set, const char **out, size_t cap);

#ifdef __cplusplus
}
#endif
