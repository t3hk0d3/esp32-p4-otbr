#include "self_update_priv.h"

#include <stdio.h>
#include <string.h>

#define TAG_MAX 32
#define VERSION_MAX 31
#define STAMP_LEN 16
#define HASH_LEN 7

static bool is_digit(char c)
{
    return c >= '0' && c <= '9';
}

static bool is_hex(char c)
{
    return is_digit(c) || (c >= 'a' && c <= 'f');
}

static bool tag_valid(const char *tag, size_t len)
{
    if (len < 1 || len > TAG_MAX) {
        return false;
    }
    if (tag[0] == '.' && (len == 1 || (len == 2 && tag[1] == '.'))) {
        return false; // "." and ".." are dot segments in the asset path
    }
    for (size_t i = 0; i < len; i++) {
        char c = tag[i];
        bool ok = is_digit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == '_' || c == '-';
        if (!ok) {
            return false;
        }
    }
    return true;
}

/** If `line` starts with `key` and ':', return the value start (spaces skipped); else NULL. */
static const char *line_value(const char *line, const char *key)
{
    size_t n = strlen(key);
    if (strncmp(line, key, n) != 0 || line[n] != ':') {
        return NULL;
    }
    const char *v = line + n + 1;
    while (*v == ' ' || *v == '\t') {
        v++;
    }
    return v;
}

static size_t trimmed_len(const char *v)
{
    size_t n = strlen(v);
    while (n > 0 && (v[n - 1] == ' ' || v[n - 1] == '\t' || v[n - 1] == '\r')) {
        n--;
    }
    return n;
}

esp_err_t self_update_parse_versions(const char *text, size_t len, self_update_release_t *out)
{
    if (!text || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len > SELF_UPDATE_VERSIONS_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (memchr(text, '\0', len)) {
        return ESP_ERR_INVALID_ARG;
    }
    char buf[SELF_UPDATE_VERSIONS_MAX + 1];
    memcpy(buf, text, len);
    buf[len] = '\0';

    self_update_release_t found = {0};
    bool seen_tag = false, seen_app = false, seen_rcp = false;
    char *line = buf;
    while (*line) {
        char *eol = strchr(line, '\n');
        if (eol) {
            *eol = '\0';
        }
        const char *v;
        if (!seen_tag && (v = line_value(line, "release"))) {
            seen_tag = true;
            size_t n = trimmed_len(v);
            if (!tag_valid(v, n)) {
                return ESP_ERR_INVALID_ARG;
            }
            memcpy(found.tag, v, n);
        } else if (!seen_app && (v = line_value(line, "p4 app"))) {
            seen_app = true;
            size_t n = trimmed_len(v);
            if (n > VERSION_MAX) {
                return ESP_ERR_INVALID_SIZE;
            }
            memcpy(found.app, v, n);
        } else if (!seen_rcp && (v = line_value(line, "c6_rcp"))) {
            seen_rcp = true;
            size_t n = trimmed_len(v);
            if (n > VERSION_MAX) {
                return ESP_ERR_INVALID_SIZE;
            }
            memcpy(found.rcp, v, n);
        }
        if (!eol) {
            break;
        }
        line = eol + 1;
    }
    if (found.app[0] == '\0') {
        return ESP_ERR_NOT_FOUND;
    }
    *out = found;
    return ESP_OK;
}

/** YYYYMMDDTHHMMSSZ-<7 hex> with an optional -dirty. */
static bool version_valid(const char *v)
{
    if (!v) {
        return false;
    }
    for (int i = 0; i < STAMP_LEN; i++) {
        bool ok = (i == 8) ? v[i] == 'T' : (i == STAMP_LEN - 1) ? v[i] == 'Z' : is_digit(v[i]);
        if (!ok) {
            return false;
        }
    }
    if (v[STAMP_LEN] != '-') {
        return false;
    }
    for (int i = 0; i < HASH_LEN; i++) {
        if (!is_hex(v[STAMP_LEN + 1 + i])) {
            return false;
        }
    }
    const char *rest = v + STAMP_LEN + 1 + HASH_LEN;
    return *rest == '\0' || strcmp(rest, "-dirty") == 0;
}

self_update_cmp_t self_update_compare(const char *release_app, const char *running_app)
{
    if (!version_valid(release_app) || !version_valid(running_app)) {
        return SELF_UPDATE_UNKNOWN;
    }
    int c = strncmp(release_app, running_app, STAMP_LEN);
    if (c > 0) {
        return SELF_UPDATE_NEWER;
    }
    if (c < 0) {
        return SELF_UPDATE_OLDER;
    }
    bool same_hash = strncmp(release_app + STAMP_LEN, running_app + STAMP_LEN, 1 + HASH_LEN) == 0;
    return same_hash ? SELF_UPDATE_SAME : SELF_UPDATE_UNKNOWN;
}

bool self_update_rcp_differs(const char *release_rcp, const char *running_rcp, bool running_known)
{
    if (!release_rcp || release_rcp[0] == '\0') {
        return false;
    }
    if (!running_known || !running_rcp || running_rcp[0] == '\0') {
        return true;
    }
    return strcmp(release_rcp, running_rcp) != 0;
}

static esp_err_t finish_url(int n, char *out, size_t cap)
{
    if (n < 0 || (size_t)n >= cap) {
        if (cap > 0) {
            out[0] = '\0';
        }
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

esp_err_t self_update_asset_url(const char *base, const char *tag, const char *file, char *out, size_t cap)
{
    if (!out || cap == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';
    if (!base || !tag || !file) {
        return ESP_ERR_INVALID_ARG;
    }
    if ((tag[0] != '\0' && !tag_valid(tag, strlen(tag))) || !tag_valid(file, strlen(file))) {
        return ESP_ERR_INVALID_ARG;
    }
    int n;
    if (tag[0]) {
        n = snprintf(out, cap, "%s/download/%s/%s", base, tag, file);
    } else {
        n = snprintf(out, cap, "%s/latest/download/%s", base, file);
    }
    return finish_url(n, out, cap);
}

esp_err_t self_update_release_page_url(const char *base, const char *tag, char *out, size_t cap)
{
    if (!out || cap == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';
    if (!base || !tag || (tag[0] != '\0' && !tag_valid(tag, strlen(tag)))) {
        return ESP_ERR_INVALID_ARG;
    }
    int n = tag[0] ? snprintf(out, cap, "%s/tag/%s", base, tag) : snprintf(out, cap, "%s/latest", base);
    return finish_url(n, out, cap);
}
