// /device/logging bodies and the live log WebSocket's text (spec docs/superpowers/specs/2026-11-10-logging-design.md,
// "Interfaces"). Pure.
#include "logging_json.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "device_api_priv.h"
#include "logging_core.h"
#include "logging_text.h"

static const char *const CHANNEL_NAMES[LOGGING_CH_COUNT] = {"uart", "jtag", "syslog"};
static const char *const SYSLOG_STATES[] = {"off", "resolving", "sending", "unresolved", "no_socket"};
static const char LEVEL_LETTERS[] = "NEWIDV"; // indexed by esp_log_level_t

// The field a failed parse names; "components." plus a tag of any length the client sent, cut to fit.
static char s_bad_field[sizeof("components.") + 64];

static void counter_pair(json_out_t *w, const char *name, uint32_t sent, uint32_t dropped)
{
    json_out_str(w, name);
    json_out_raw(w, ":{\"sent\":");
    json_out_uint(w, sent);
    json_out_raw(w, ",\"dropped\":");
    json_out_uint(w, dropped);
    json_out_raw(w, "}");
}

int device_api_logging_json(char *out, size_t cap, const logging_settings_t *settings, const logging_status_t *status,
                            const char **known, size_t n_known)
{
    json_out_t w;
    const unsigned state = (unsigned) status->syslog;

    json_out_init(&w, out, cap);
    json_out_raw(&w, "{\"max_level\":");
    json_out_str(&w, logging_level_name(logging_level_ceiling()));
    json_out_raw(&w, ",\"channels\":{\"uart\":{\"level\":");
    json_out_str(&w, logging_level_name(settings->channel[LOGGING_CH_UART]));
    json_out_raw(&w, "},\"jtag\":{\"level\":");
    json_out_str(&w, logging_level_name(settings->channel[LOGGING_CH_JTAG]));
    json_out_raw(&w, ",\"host\":");
    json_out_bool(&w, status->jtag_host);
    json_out_raw(&w, "},\"syslog\":{\"level\":");
    json_out_str(&w, logging_level_name(settings->channel[LOGGING_CH_SYSLOG]));
    json_out_raw(&w, ",\"target\":");
    json_out_str(&w, settings->syslog_target);
    json_out_raw(&w, ",\"state\":");
    json_out_str(&w, state < sizeof(SYSLOG_STATES) / sizeof(SYSLOG_STATES[0]) ? SYSLOG_STATES[state] : "off");
    json_out_raw(&w, "}},\"components\":{\"*\":");
    json_out_str(&w, logging_level_name(settings->default_level));
    for (size_t i = 0; i < settings->n_components && i < LOGGING_COMPONENTS_MAX; i++) {
        json_out_raw(&w, ",");
        json_out_str(&w, settings->components[i].tag);
        json_out_raw(&w, ":");
        json_out_str(&w, logging_level_name(settings->components[i].level));
    }
    json_out_raw(&w, "},\"known_tags\":[");
    for (size_t i = 0; i < n_known; i++) {
        if (i) {
            json_out_raw(&w, ",");
        }
        json_out_str(&w, known[i]);
    }
    json_out_raw(&w, "],\"viewers\":");
    json_out_uint(&w, status->viewers);
    json_out_raw(&w, ",\"counters\":{");
    for (int ch = 0; ch < LOGGING_CH_COUNT; ch++) {
        counter_pair(&w, CHANNEL_NAMES[ch], status->sent[ch], status->dropped[ch]);
        json_out_raw(&w, ",");
    }
    counter_pair(&w, "live", status->live_sent, status->live_dropped);
    json_out_raw(&w, "}}");
    return w.overflow ? -1 : (int) w.len;
}

static esp_err_t bad(const char *field, const char **bad_field)
{
    strlcpy(s_bad_field, field, sizeof(s_bad_field));
    if (bad_field) {
        *bad_field = s_bad_field;
    }
    return ESP_ERR_INVALID_ARG;
}

static esp_err_t bad_component(const char *tag, const char **bad_field)
{
    snprintf(s_bad_field, sizeof(s_bad_field), "components.%s", tag);
    if (bad_field) {
        *bad_field = s_bad_field;
    }
    return ESP_ERR_INVALID_ARG;
}

/** A level string at `item`; INVALID_ARG for anything else. */
static esp_err_t level_of(const cJSON *item, esp_log_level_t *out)
{
    return cJSON_IsString(item) ? logging_level_parse(item->valuestring, out) : ESP_ERR_INVALID_ARG;
}

/** A live viewer's level: a level name up to the build's ceiling; INVALID_ARG for anything else (`*out` untouched). */
static esp_err_t level_named(const char *name, esp_log_level_t *out)
{
    esp_log_level_t level;

    if (logging_level_parse(name, &level) != ESP_OK || level > logging_level_ceiling()) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = level;
    return ESP_OK;
}

static esp_err_t parse_channels(const cJSON *channels, logging_settings_t *s, const char **bad_field)
{
    char field[sizeof("channels.syslog.target")];

    if (!cJSON_IsObject(channels)) {
        return bad("channels", bad_field);
    }
    for (int ch = 0; ch < LOGGING_CH_COUNT; ch++) {
        const cJSON *c = cJSON_GetObjectItemCaseSensitive(channels, CHANNEL_NAMES[ch]);
        const cJSON *level;

        if (!c) {
            continue;
        }
        if (!cJSON_IsObject(c)) {
            snprintf(field, sizeof(field), "channels.%s", CHANNEL_NAMES[ch]);
            return bad(field, bad_field);
        }
        level = cJSON_GetObjectItemCaseSensitive(c, "level");
        if (level && level_of(level, &s->channel[ch]) != ESP_OK) {
            snprintf(field, sizeof(field), "channels.%s.level", CHANNEL_NAMES[ch]);
            return bad(field, bad_field);
        }
    }
    const cJSON *syslog = cJSON_GetObjectItemCaseSensitive(channels, "syslog");
    const cJSON *target = syslog ? cJSON_GetObjectItemCaseSensitive(syslog, "target") : NULL;

    if (target) {
        char host[LOGGING_TARGET_MAX];
        uint16_t port;

        if (!cJSON_IsString(target) || strlen(target->valuestring) >= LOGGING_TARGET_MAX ||
                (target->valuestring[0] != '\0' &&
                 logging_target_parse(target->valuestring, host, sizeof(host), &port) != ESP_OK)) {
            return bad("channels.syslog.target", bad_field);
        }
        strlcpy(s->syslog_target, target->valuestring, sizeof(s->syslog_target));
    }
    return ESP_OK;
}

static esp_err_t parse_components(const cJSON *components, logging_settings_t *s, const char **bad_field)
{
    const cJSON *item;

    bool star = false;

    if (!cJSON_IsObject(components)) {
        return bad("components", bad_field);
    }
    s->n_components = 0;
    cJSON_ArrayForEach(item, components) {
        esp_log_level_t level;
        esp_err_t err;

        if (level_of(item, &level) != ESP_OK) {
            return bad_component(item->string, bad_field);
        }
        if (strcmp(item->string, "*") == 0) {
            if (star) {
                return bad_component(item->string, bad_field); // listed twice (Ruling R5)
            }
            star = true;
            s->default_level = level;
            continue;
        }
        for (size_t i = 0; i < s->n_components; i++) {
            if (strcmp(s->components[i].tag, item->string) == 0) {
                return bad_component(item->string, bad_field); // listed twice (Ruling R5)
            }
        }
        err = logging_core_set_component(s, item->string, level);
        if (err == ESP_ERR_NO_MEM) {
            return bad("components", bad_field);
        }
        if (err != ESP_OK) {
            return bad_component(item->string, bad_field);
        }
    }
    return ESP_OK;
}

// Unknown keys (top level and inside a channel) are ignored, as GET's read-only fields are: a client may PUT back what
// GET gave it.
esp_err_t device_api_logging_parse(const char *body, logging_settings_t *inout, const char **bad_field)
{
    cJSON *json = cJSON_Parse(body);
    logging_settings_t *next;
    esp_err_t err = ESP_OK;

    if (!cJSON_IsObject(json)) {
        cJSON_Delete(json);
        return bad("body", bad_field);
    }
    next = malloc(sizeof(*next)); // off the server task's stack
    if (!next) {
        cJSON_Delete(json);
        return ESP_ERR_NO_MEM;
    }
    memcpy(next, inout, sizeof(*next));

    const cJSON *channels = cJSON_GetObjectItemCaseSensitive(json, "channels");
    const cJSON *components = cJSON_GetObjectItemCaseSensitive(json, "components");

    if (channels) {
        err = parse_channels(channels, next, bad_field);
    }
    if (err == ESP_OK && components) {
        err = parse_components(components, next, bad_field);
    }
    if (err == ESP_OK) {
        memcpy(inout, next, sizeof(*inout));
    }
    free(next);
    cJSON_Delete(json);
    return err;
}

/**
 * Length of the well-formed UTF-8 sequence (RFC 3629) at `s`, `n` bytes left; 0 when there is none, and `*bad` then
 * gets the length of the maximal ill-formed subpart (at least 1), which becomes one U+FFFD (Unicode's "substitution of
 * maximal subparts").
 */
static size_t utf8_sequence(const unsigned char *s, size_t n, size_t *bad)
{
    unsigned char lo = 0x80;
    unsigned char hi = 0xbf;
    size_t need;

    if (s[0] < 0x80) {
        return 1;
    }
    if (s[0] >= 0xc2 && s[0] <= 0xdf) {
        need = 2;
    } else if (s[0] >= 0xe0 && s[0] <= 0xef) {
        need = 3;
        lo = s[0] == 0xe0 ? 0xa0 : 0x80; // no overlong forms
        hi = s[0] == 0xed ? 0x9f : 0xbf; // no surrogates
    } else if (s[0] >= 0xf0 && s[0] <= 0xf4) {
        need = 4;
        lo = s[0] == 0xf0 ? 0x90 : 0x80; // no overlong forms
        hi = s[0] == 0xf4 ? 0x8f : 0xbf; // nothing above U+10FFFF
    } else {
        *bad = 1; // a continuation byte, C0, C1 or F5..FF
        return 0;
    }
    for (size_t i = 1; i < need; i++) {
        if (i >= n || s[i] < lo || s[i] > hi) {
            *bad = i;
            return 0;
        }
        lo = 0x80;
        hi = 0xbf;
    }
    return need;
}

/** Append `len` bytes of `text` at `*pos`, each ill-formed UTF-8 subpart as U+FFFD. False when `cap` is too small. */
static bool append_utf8(char *out, size_t cap, size_t *pos, const char *text, size_t len)
{
    static const char REPLACEMENT[] = "\xef\xbf\xbd"; // U+FFFD
    const unsigned char *s = (const unsigned char *) text;
    size_t i = 0;

    while (i < len) {
        size_t bad = 0;
        size_t good = utf8_sequence(s + i, len - i, &bad);
        const char *from = good ? text + i : REPLACEMENT;
        size_t n = good ? good : sizeof(REPLACEMENT) - 1;

        if (cap - *pos <= n) {
            return false;
        }
        memcpy(out + *pos, from, n);
        *pos += n;
        i += good ? good : bad;
    }
    return true;
}

int device_api_logs_line(char *out, size_t cap, const logging_line_t *line)
{
    const unsigned level = (unsigned) line->level;
    int n = snprintf(out, cap, "%c (%" PRIu32 ") ", level < sizeof(LEVEL_LETTERS) - 1 ? LEVEL_LETTERS[level] : '?',
                     line->uptime_ms);
    size_t pos;

    if (n < 0 || (size_t) n >= cap) {
        return -1;
    }
    pos = (size_t) n;
    // A text frame must be valid UTF-8 (RFC 6455 5.6): a client closes the connection on anything else.
    if (!append_utf8(out, cap, &pos, line->tag, strnlen(line->tag, sizeof(line->tag))) ||
            !append_utf8(out, cap, &pos, ": ", 2) || !append_utf8(out, cap, &pos, line->text, line->len)) {
        return -1;
    }
    out[pos] = '\0';
    return (int) pos;
}

int device_api_logs_dropped(char *out, size_t cap, uint32_t n)
{
    int len = snprintf(out, cap, "# dropped %" PRIu32 " lines", n);

    return len < 0 || (size_t) len >= cap ? -1 : len;
}

esp_err_t device_api_logs_query_level(const char *query, esp_log_level_t *out)
{
    char name[16];
    esp_err_t err;

    if (!query) {
        *out = ESP_LOG_INFO;
        return ESP_OK;
    }
    err = device_api_query_value(query, "level", name, sizeof(name));
    if (err == ESP_ERR_NOT_FOUND) {
        *out = ESP_LOG_INFO;
        return ESP_OK;
    }
    return err == ESP_OK ? level_named(name, out) : ESP_ERR_INVALID_ARG;
}

esp_err_t device_api_logs_level_frame(const char *frame, size_t len, esp_log_level_t *out)
{
    cJSON *json = cJSON_ParseWithLength(frame, len);
    const cJSON *level = cJSON_IsObject(json) ? cJSON_GetObjectItemCaseSensitive(json, "level") : NULL;
    esp_err_t err = cJSON_IsString(level) ? level_named(level->valuestring, out) : ESP_ERR_INVALID_ARG;

    cJSON_Delete(json);
    return err;
}

int device_api_logs_ws_refusal(size_t open_viewers)
{
    return open_viewers >= LOGGING_VIEWERS_MAX ? 503 : 0;
}
