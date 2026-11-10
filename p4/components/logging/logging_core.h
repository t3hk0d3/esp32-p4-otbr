#pragma once

// Pure decisions of the logging component: level names, the settings model, the generation gate, validation and the
// set of tags seen since boot. Host-tested in test_host/.

#include <stdbool.h>
#include <stddef.h>
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

/** Add or replace an override. INVALID_ARG: bad tag (empty, "*", 24+ bytes) or level; NO_MEM: set is full. */
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

/** Tags seen since boot, in insertion order. */
typedef struct {
    size_t count;
    char tags[LOGGING_KNOWN_TAGS_MAX][LOGGING_TAG_MAX];
} logging_tagset_t;

/** Remember a tag. True only when it was newly added; false when known, invalid (empty, 24+ bytes) or the set is full. */
bool logging_tagset_add(logging_tagset_t *set, const char *tag);

/** Copy up to `cap` pointers (into the set) in insertion order; returns how many. */
size_t logging_tagset_list(const logging_tagset_t *set, const char **out, size_t cap);

#ifdef __cplusplus
}
#endif
