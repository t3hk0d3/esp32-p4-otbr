#include <stdio.h>
#include <string.h>
#include "logging_core.h"
#include "logging_text.h"

static const char *const LEVEL_NAMES[] = {"off", "error", "warning", "info", "debug", "verbose"};
static const char *const CHANNEL_NAMES[LOGGING_CH_COUNT] = {"uart", "jtag", "syslog"};

static bool level_ok(esp_log_level_t level)
{
    return level >= ESP_LOG_NONE && level <= ESP_LOG_VERBOSE;
}

static bool tag_ok(const char *tag)
{
    size_t len = tag ? strlen(tag) : 0;

    return len > 0 && len < LOGGING_TAG_MAX;
}

const char *logging_level_name(esp_log_level_t level)
{
    return level_ok(level) ? LEVEL_NAMES[level] : "?";
}

esp_err_t logging_level_parse(const char *name, esp_log_level_t *out)
{
    if (!name) {
        return ESP_ERR_INVALID_ARG;
    }
    for (int i = 0; i <= ESP_LOG_VERBOSE; i++) {
        if (strcmp(name, LEVEL_NAMES[i]) == 0) {
            *out = (esp_log_level_t) i;
            return ESP_OK;
        }
    }
    return ESP_ERR_INVALID_ARG;
}

void logging_core_defaults(logging_settings_t *s)
{
    memset(s, 0, sizeof(*s));
    s->channel[LOGGING_CH_UART] = ESP_LOG_INFO;
    s->channel[LOGGING_CH_JTAG] = ESP_LOG_INFO;
    s->channel[LOGGING_CH_SYSLOG] = ESP_LOG_NONE;
    s->default_level = ESP_LOG_INFO;
}

esp_log_level_t logging_core_tag_level(const logging_settings_t *s, const char *tag)
{
    for (int i = 0; i < s->n_components; i++) {
        if (strcmp(s->components[i].tag, tag) == 0) {
            return s->components[i].level;
        }
    }
    return s->default_level;
}

esp_log_level_t logging_core_channels_max(const logging_settings_t *s, esp_log_level_t viewers_max)
{
    esp_log_level_t max = viewers_max;

    for (int ch = 0; ch < LOGGING_CH_COUNT; ch++) {
        if (s->channel[ch] > max) {
            max = s->channel[ch];
        }
    }
    return max;
}

esp_log_level_t logging_core_gate(const logging_settings_t *s, const char *tag, esp_log_level_t viewers_max)
{
    esp_log_level_t component = logging_core_tag_level(s, tag);
    esp_log_level_t channels = logging_core_channels_max(s, viewers_max);

    return component < channels ? component : channels;
}

bool logging_core_passes(esp_log_level_t line, esp_log_level_t component, esp_log_level_t channel)
{
    return line <= component && line <= channel;
}

esp_err_t logging_core_set_component(logging_settings_t *s, const char *tag, esp_log_level_t level)
{
    if (!tag_ok(tag) || strcmp(tag, "*") == 0 || !level_ok(level)) {
        return ESP_ERR_INVALID_ARG;
    }
    for (int i = 0; i < s->n_components; i++) {
        if (strcmp(s->components[i].tag, tag) == 0) {
            s->components[i].level = level;
            return ESP_OK;
        }
    }
    if (s->n_components >= LOGGING_COMPONENTS_MAX) {
        return ESP_ERR_NO_MEM;
    }
    logging_component_t *c = &s->components[s->n_components++];
    strlcpy(c->tag, tag, sizeof(c->tag));
    c->level = level;
    return ESP_OK;
}

esp_err_t logging_core_remove_component(logging_settings_t *s, const char *tag)
{
    for (int i = 0; i < s->n_components; i++) {
        if (strcmp(s->components[i].tag, tag) == 0) {
            memmove(&s->components[i], &s->components[i + 1], (s->n_components - i - 1) * sizeof(s->components[0]));
            s->n_components--;
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t logging_core_validate(const logging_settings_t *s, const char **field)
{
    static char component_field[sizeof("components.") + LOGGING_TAG_MAX];
    static char channel_field[LOGGING_CH_COUNT][sizeof("channels.syslog.level")];
    static const char *const target_field = "channels.syslog.target";
    const char *bad = NULL;

    for (int ch = 0; ch < LOGGING_CH_COUNT && !bad; ch++) {
        if (!level_ok(s->channel[ch])) {
            snprintf(channel_field[ch], sizeof(channel_field[ch]), "channels.%s.level", CHANNEL_NAMES[ch]);
            bad = channel_field[ch];
        }
    }
    if (!bad && !level_ok(s->default_level)) {
        bad = "components.*";
    }
    if (!bad && s->n_components > LOGGING_COMPONENTS_MAX) {
        bad = "components";
    }
    for (int i = 0; i < s->n_components && !bad; i++) {
        const logging_component_t *c = &s->components[i];
        bool terminated = memchr(c->tag, '\0', sizeof(c->tag)) != NULL;
        bool ok = terminated && level_ok(c->level) && tag_ok(c->tag) && strcmp(c->tag, "*") != 0;

        for (int j = 0; j < i && ok; j++) {
            ok = strcmp(s->components[j].tag, c->tag) != 0;
        }
        if (!ok) {
            snprintf(component_field, sizeof(component_field), "components.%.*s", LOGGING_TAG_MAX - 1, c->tag);
            bad = component_field;
        }
    }
    if (!bad) {
        char host[LOGGING_TARGET_MAX];
        uint16_t port;
        bool terminated = memchr(s->syslog_target, '\0', sizeof(s->syslog_target)) != NULL;

        if (s->syslog_target[0] != '\0' || s->channel[LOGGING_CH_SYSLOG] > ESP_LOG_NONE) {
            if (!terminated || logging_target_parse(s->syslog_target, host, sizeof(host), &port) != ESP_OK) {
                bad = target_field;
            }
        }
    }
    if (bad) {
        if (field) {
            *field = bad;
        }
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

bool logging_tagset_add(logging_tagset_t *set, const char *tag)
{
    size_t count = set->count; // only the writer changes it

    if (!tag_ok(tag) || count >= LOGGING_KNOWN_TAGS_MAX) {
        return false;
    }
    for (size_t i = 0; i < count; i++) {
        if (strcmp(set->tags[i], tag) == 0) {
            return false;
        }
    }
    strlcpy(set->tags[count], tag, LOGGING_TAG_MAX);
    // Publishes the entry above to lock-free readers (logging_tagset_list).
    __atomic_store_n(&set->count, count + 1, __ATOMIC_RELEASE);
    return true;
}

size_t logging_tagset_list(const logging_tagset_t *set, const char **out, size_t cap)
{
    size_t count = __atomic_load_n(&set->count, __ATOMIC_ACQUIRE);
    size_t n = count < cap ? count : cap;

    for (size_t i = 0; i < n; i++) {
        out[i] = set->tags[i];
    }
    return n;
}

logging_levels_t logging_core_levels(const logging_settings_t *s, esp_log_level_t viewers_max)
{
    logging_levels_t lv = {.word = 0};

    for (int ch = 0; ch < LOGGING_CH_COUNT; ch++) {
        lv.channel[ch] = (uint8_t) s->channel[ch];
    }
    lv.viewers = (uint8_t) viewers_max;
    return lv;
}

esp_log_level_t logging_core_nonuart_max(logging_levels_t lv)
{
    uint8_t max = lv.viewers;

    for (int ch = 0; ch < LOGGING_CH_COUNT; ch++) {
        if (ch != LOGGING_CH_UART && lv.channel[ch] > max) {
            max = lv.channel[ch];
        }
    }
    return (esp_log_level_t) max;
}

esp_log_level_t logging_core_live_cap(const char *tag, esp_log_level_t viewer)
{
    return tag && strncmp(tag, "httpd", 5) == 0 && viewer > ESP_LOG_INFO ? ESP_LOG_INFO : viewer;
}

unsigned logging_capture_route(esp_log_level_t level, esp_log_level_t uart_level, esp_log_level_t nonuart_max,
                               esp_log_level_t tag_level, bool in_guard, bool in_isr)
{
    unsigned route = 0;

    if (in_isr) {
        return LOGGING_ROUTE_UART;
    }
    if (level == ESP_LOG_NONE) {
        return 0;
    }
    if (level <= uart_level) {
        route |= LOGGING_ROUTE_UART;
    }
    if (!in_guard && level <= nonuart_max && level <= tag_level) {
        route |= LOGGING_ROUTE_RING;
    }
    return route;
}

unsigned logging_core_wanting(esp_log_level_t level, logging_levels_t lv)
{
    unsigned want = 0;

    if (level == ESP_LOG_NONE) {
        return 0;
    }
    for (int ch = 0; ch < LOGGING_CH_COUNT; ch++) {
        if (ch != LOGGING_CH_UART && level <= lv.channel[ch]) {
            want |= 1u << ch;
        }
    }
    if (level <= lv.viewers) {
        want |= LOGGING_WANT_LIVE;
    }
    return want;
}

/* ---- console command ---- */

static esp_err_t console_fail(const char **err, const char *message, esp_err_t code)
{
    if (err) {
        *err = message;
    }
    return code;
}

static esp_err_t console_level(int argc, char **argv, logging_settings_t *s, const char **err)
{
    esp_log_level_t level;
    int ch = -1;

    if (argc != 4) {
        return console_fail(err, "usage: log level <uart|jtag|syslog> <level>", ESP_ERR_INVALID_ARG);
    }
    for (int i = 0; i < LOGGING_CH_COUNT; i++) {
        if (strcmp(argv[2], CHANNEL_NAMES[i]) == 0) {
            ch = i;
        }
    }
    if (ch < 0) {
        return console_fail(err, "unknown channel (uart, jtag or syslog)", ESP_ERR_INVALID_ARG);
    }
    if (logging_level_parse(argv[3], &level) != ESP_OK) {
        return console_fail(err, "unknown level (off, error, warning, info, debug or verbose)", ESP_ERR_INVALID_ARG);
    }
    s->channel[ch] = level;
    return ESP_OK;
}

static esp_err_t console_tag(int argc, char **argv, logging_settings_t *s, const char **err)
{
    const char *tag = argv[2];
    esp_log_level_t level;
    bool is_default;
    esp_err_t ret;

    if (argc != 4) {
        return console_fail(err, "usage: log tag <tag|*> <level|default>", ESP_ERR_INVALID_ARG);
    }
    is_default = strcmp(argv[3], "default") == 0;
    if (!is_default && logging_level_parse(argv[3], &level) != ESP_OK) {
        return console_fail(err, "unknown level (off, error, warning, info, debug, verbose or default)",
                            ESP_ERR_INVALID_ARG);
    }
    if (strcmp(tag, "*") == 0) {
        if (is_default) {
            return console_fail(err, "* is the default for everything else; give it a level",
                                ESP_ERR_INVALID_ARG);
        }
        s->default_level = level;
        return ESP_OK;
    }
    if (is_default) {
        if (logging_core_remove_component(s, tag) != ESP_OK) {
            return console_fail(err, "that tag has no override", ESP_ERR_INVALID_ARG);
        }
        return ESP_OK;
    }
    ret = logging_core_set_component(s, tag, level);
    if (ret == ESP_ERR_NO_MEM) {
        return console_fail(err, "too many overrides (32); remove one with: log tag <tag> default", ret);
    }
    if (ret != ESP_OK) {
        return console_fail(err, "bad tag (1 to 23 bytes, not *)", ESP_ERR_INVALID_ARG);
    }
    return ESP_OK;
}

static esp_err_t console_syslog(int argc, char **argv, logging_settings_t *s, const char **err)
{
    char host[LOGGING_TARGET_MAX];
    uint16_t port;

    if (argc != 3) {
        return console_fail(err, "usage: log syslog <host[:port]|off>", ESP_ERR_INVALID_ARG);
    }
    if (strcmp(argv[2], "off") == 0) {
        s->syslog_target[0] = '\0';
        s->channel[LOGGING_CH_SYSLOG] = ESP_LOG_NONE;
        return ESP_OK;
    }
    if (strlen(argv[2]) >= sizeof(s->syslog_target) ||
            logging_target_parse(argv[2], host, sizeof(host), &port) != ESP_OK) {
        return console_fail(err, "bad target (host, host:port, [ipv6] or [ipv6]:port)", ESP_ERR_INVALID_ARG);
    }
    strcpy(s->syslog_target, argv[2]);
    return ESP_OK;
}

esp_err_t logging_console_parse(int argc, char **argv, logging_settings_t *inout, const char **err)
{
    logging_settings_t work = *inout; // the caller's struct changes only when the whole command is good
    esp_err_t ret;

    if (argc < 2) {
        return console_fail(err, "usage: log [level|tag|syslog] ...", ESP_ERR_INVALID_ARG);
    }
    if (strcmp(argv[1], "level") == 0) {
        ret = console_level(argc, argv, &work, err);
    } else if (strcmp(argv[1], "tag") == 0) {
        ret = console_tag(argc, argv, &work, err);
    } else if (strcmp(argv[1], "syslog") == 0) {
        ret = console_syslog(argc, argv, &work, err);
    } else {
        return console_fail(err, "unknown subcommand (level, tag or syslog)", ESP_ERR_INVALID_ARG);
    }
    if (ret == ESP_OK) {
        *inout = work;
    }
    return ret;
}
