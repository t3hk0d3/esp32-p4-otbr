#pragma once

// Pure helpers of self_update (no HTTP, no ESP-IDF drivers): host-tested in test_host/.

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "self_update.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- self_update_core.c ---- */

/**
 * Parse the text of a release's versions.txt (not necessarily NUL-terminated; never read past `len`).
 * Keys `release`, `p4 app` and `c6_rcp`, each followed by ':'; other lines are ignored; the first occurrence of a key
 * wins; "\r\n" is accepted. `out` is written only on ESP_OK.
 * ESP_ERR_INVALID_SIZE: len above SELF_UPDATE_VERSIONS_MAX, or a version longer than 31 characters.
 * ESP_ERR_INVALID_ARG: NULL argument, a NUL inside the text, or a tag that is not 1 to 32 of [A-Za-z0-9._-].
 * ESP_ERR_NOT_FOUND: no `p4 app` line, or an empty value.
 */
esp_err_t self_update_parse_versions(const char *text, size_t len, self_update_release_t *out);

/** Order two versions (YYYYMMDDTHHMMSSZ-<7 hex>[-dirty]) by their stamp. */
self_update_cmp_t self_update_compare(const char *release_app, const char *running_app);

/** Whether the radio firmware must be installed: the release has one and it is not the installed one. */
bool self_update_rcp_differs(const char *release_rcp, const char *running_rcp, bool running_known);

/** "<base>/download/<tag>/<file>", or "<base>/latest/download/<file>" for an empty tag. */
esp_err_t self_update_asset_url(const char *base, const char *tag, const char *file, char *out, size_t cap);

/** "<base>/tag/<tag>", or "<base>/latest" for an empty tag. */
esp_err_t self_update_release_page_url(const char *base, const char *tag, char *out, size_t cap);

/* ---- self_update.c ---- */

/** Network and clock seam: the target defaults live in self_update_http.c, host tests set fakes. */
typedef struct {
    /**
     * GET `url` into `buf` (at most `cap` bytes), the length in `*len`. On failure `detail` says why and names the
     * URL asked for.
     */
    esp_err_t (*fetch)(const char *url, char *buf, size_t cap, size_t *len, char *detail, size_t detail_cap);
    int64_t (*now_us)(void);
} self_update_ops_t;

void self_update_set_ops(const self_update_ops_t *ops);

#ifndef CONFIG_IDF_TARGET_LINUX
/** Default ops of the target (self_update_http.c): esp_http_client GET and esp_timer. */
extern const self_update_ops_t self_update_http_ops;
#endif

/** Back to the never-checked state (host tests). */
void self_update_reset(void);

#ifdef __cplusplus
}
#endif
