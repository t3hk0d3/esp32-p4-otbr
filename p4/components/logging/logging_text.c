#include <stdio.h>
#include <string.h>
#include <time.h>
#include "logging.h"
#include "logging_text.h"

static bool host_char_ok(char c)
{
    return (unsigned char) c > ' ' && c != 0x7f;
}

static bool parse_port(const char *s, uint16_t *port)
{
    unsigned long v = 0;

    if (*s == '\0') {
        return false;
    }
    for (; *s; s++) {
        if (*s < '0' || *s > '9') {
            return false;
        }
        v = v * 10 + (unsigned long)(*s - '0');
        if (v > 65535) {
            return false;
        }
    }
    if (v == 0) {
        return false;
    }
    *port = (uint16_t) v;
    return true;
}

esp_err_t logging_target_parse(const char *in, char *host, size_t host_cap, uint16_t *port)
{
    const char *h;
    size_t hlen;
    uint16_t p = LOGGING_SYSLOG_PORT;

    if (!in || !host || !port) {
        return ESP_ERR_INVALID_ARG;
    }
    if (in[0] == '[') {
        const char *close = strchr(in, ']');

        if (!close) {
            return ESP_ERR_INVALID_ARG;
        }
        h = in + 1;
        hlen = (size_t)(close - h);
        if (close[1] == ':') {
            if (!parse_port(close + 2, &p)) {
                return ESP_ERR_INVALID_ARG;
            }
        } else if (close[1] != '\0') {
            return ESP_ERR_INVALID_ARG;
        }
    } else {
        const char *colon = strchr(in, ':');

        h = in;
        if (colon) {
            if (strchr(colon + 1, ':') || !parse_port(colon + 1, &p)) {
                return ESP_ERR_INVALID_ARG; /* a second colon is an unbracketed IPv6 address */
            }
            hlen = (size_t)(colon - in);
        } else {
            hlen = strlen(in);
        }
    }
    if (hlen == 0 || hlen >= host_cap) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < hlen; i++) {
        if (!host_char_ok(h[i]) || h[i] == '[' || h[i] == ']') {
            return ESP_ERR_INVALID_ARG;
        }
    }
    memcpy(host, h, hlen);
    host[hlen] = '\0';
    *port = p;
    return ESP_OK;
}

#define ANSI_RESET "\033[0m"
#define CUT_MARKER "\xe2\x80\xa6[cut]"
#define APP_NAME_MAX 48
#define HOSTNAME_MAX 255

/* Length of "ESC [ ... m" at the start of s, or 0. */
static size_t ansi_colour_len(const char *s, size_t len)
{
    if (len < 3 || s[0] != '\033' || s[1] != '[') {
        return 0;
    }
    for (size_t i = 2; i < len && i < 12; i++) {
        if (s[i] == 'm') {
            return i + 1;
        }
        if (!((s[i] >= '0' && s[i] <= '9') || s[i] == ';')) {
            return 0;
        }
    }
    return 0;
}

/* Length of "X (<digits>) <tag>: " at the start of s, or 0 when s does not start with it. */
static size_t line_prefix_len(const char *s, size_t len)
{
    size_t i = 0;

    if (len < 4 || !strchr("EWIDV", s[0]) || s[1] != ' ' || s[2] != '(') {
        return 0;
    }
    for (i = 3; i < len && s[i] >= '0' && s[i] <= '9'; i++) {
    }
    if (i == 3 || i + 2 >= len || s[i] != ')' || s[i + 1] != ' ') {
        return 0;
    }
    for (i += 2; i + 1 < len; i++) {
        if (s[i] == ':' && s[i + 1] == ' ') {
            return i + 2;
        }
        if (s[i] == ' ' || s[i] == '\n') {
            return 0; /* a tag has no spaces */
        }
    }
    return 0;
}

size_t logging_text_body(const char *text, size_t len, const char **body)
{
    size_t colour = ansi_colour_len(text, len);
    size_t prefix = line_prefix_len(text + colour, len - colour);
    size_t start = prefix ? colour + prefix : 0;
    const size_t reset = sizeof(ANSI_RESET) - 1;

    while (len > start && (text[len - 1] == '\n' || text[len - 1] == '\r')) {
        len--;
    }
    if (prefix && len >= start + reset && memcmp(text + len - reset, ANSI_RESET, reset) == 0) {
        len -= reset;
    }
    *body = text + start;
    len -= start;
    return len > LOGGING_LINE_MAX + 1 ? LOGGING_LINE_MAX + 1 : len;
}

size_t logging_text_truncate(char *buf, size_t len, size_t cap)
{
    const size_t marker = sizeof(CUT_MARKER) - 1;
    size_t keep;

    if (len <= cap) {
        return len;
    }
    if (cap < marker) {
        return cap;
    }
    keep = cap - marker;
    while (keep > 0 && ((unsigned char) buf[keep] & 0xc0) == 0x80) {
        keep--; /* do not split a UTF-8 sequence: drop it whole */
    }
    memcpy(buf + keep, CUT_MARKER, marker);
    return keep + marker;
}

static int severity(esp_log_level_t level)
{
    switch (level) {
    case ESP_LOG_ERROR:
        return 3;
    case ESP_LOG_WARN:
        return 4;
    case ESP_LOG_INFO:
        return 6;
    default:
        return 7;
    }
}

/* Printable ASCII without spaces, cut at max; "-" when nothing is left. Returns the length. */
static size_t sanitize(char *dst, const char *src, size_t max)
{
    size_t n = 0;

    for (; src && src[n] && n < max; n++) {
        dst[n] = (src[n] > ' ' && src[n] < 0x7f) ? src[n] : '_';
    }
    if (n == 0) {
        dst[n++] = '-';
    }
    dst[n] = '\0';
    return n;
}

int logging_syslog_format(char *out, size_t cap, esp_log_level_t level, const char *hostname, const char *tag,
                          uint32_t uptime_ms, const struct timeval *wall, const char *msg, size_t msg_len)
{
    char host[HOSTNAME_MAX + 1];
    char app[APP_NAME_MAX + 1];
    char stamp[96] = "-"; /* sized for the compiler's worst case */
    size_t used;
    int n;

    if (!out || cap == 0) {
        return -1;
    }
    sanitize(host, hostname, HOSTNAME_MAX);
    sanitize(app, tag, APP_NAME_MAX);
    if (wall) {
        struct tm tm;
        time_t sec = wall->tv_sec;

        if (gmtime_r(&sec, &tm)) {
            snprintf(stamp, sizeof(stamp), "%04d-%02d-%02dT%02d:%02d:%02d.%03ldZ", tm.tm_year + 1900, tm.tm_mon + 1,
                     tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, (long)(wall->tv_usec / 1000) % 1000);
        }
    }
    n = snprintf(out, cap, "<%d>1 %s %s %s - - - %lu ", 16 * 8 + severity(level), stamp, host, app,
                 (unsigned long) uptime_ms);
    if (n < 0 || (size_t) n >= cap || msg_len >= cap - (size_t) n) {
        out[cap - 1] = '\0';
        return -1;
    }
    used = (size_t) n;
    if (msg_len) {
        memcpy(out + used, msg, msg_len);
    }
    out[used + msg_len] = '\0';
    return (int)(used + msg_len);
}
