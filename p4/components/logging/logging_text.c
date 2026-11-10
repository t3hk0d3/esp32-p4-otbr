#include <string.h>
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
