#include <ctype.h>
#include <string.h>
#include "device_api_priv.h"

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

esp_err_t device_api_query_value(const char *query, const char *key, char *out, size_t cap)
{
    if (key == NULL || out == NULL || cap == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (query == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    const size_t key_len = strlen(key);
    const char *p = query;

    while (*p) {
        const char *end = strchr(p, '&');
        const size_t pair_len = end ? (size_t)(end - p) : strlen(p);

        if (pair_len >= key_len && strncmp(p, key, key_len) == 0 && (pair_len == key_len || p[key_len] == '=')) {
            const char *v = p + key_len + (pair_len > key_len ? 1 : 0);
            const char *v_end = p + pair_len;
            size_t n = 0;

            while (v < v_end) {
                char c = *v;
                if (c == '%') {
                    if (v_end - v < 3 || hex_value(v[1]) < 0 || hex_value(v[2]) < 0) {
                        return ESP_ERR_INVALID_ARG;
                    }
                    c = (char)(hex_value(v[1]) * 16 + hex_value(v[2]));
                    v += 2;
                }
                if (n + 1 >= cap) {
                    return ESP_ERR_INVALID_SIZE;
                }
                out[n++] = c;
                v++;
            }
            out[n] = '\0';
            return ESP_OK;
        }
        p += pair_len;
        if (*p == '&') {
            p++;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t device_api_query_uint(const char *query, const char *key, uint32_t min, uint32_t max, uint32_t *out)
{
    char text[16];
    esp_err_t err = device_api_query_value(query, key, text, sizeof(text));

    if (err == ESP_ERR_INVALID_SIZE) {
        return ESP_ERR_INVALID_ARG; // longer than any number in range
    }
    if (err != ESP_OK) {
        return err;
    }
    if (text[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    uint64_t value = 0;

    for (const char *c = text; *c; c++) {
        if (!isdigit((unsigned char) * c)) {
            return ESP_ERR_INVALID_ARG;
        }
        value = value * 10 + (uint64_t)(*c - '0');
        if (value > max) {
            return ESP_ERR_INVALID_ARG;
        }
    }
    if (value < min) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = (uint32_t)value;
    return ESP_OK;
}
