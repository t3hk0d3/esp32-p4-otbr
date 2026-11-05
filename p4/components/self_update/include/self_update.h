#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** The repository releases come from; fixed at build time. */
#define SELF_UPDATE_REPO_URL "https://github.com/t3hk0d3/esp32-p4-otbr"
/** Base of every release URL (test builds can override it at run time, the repo link stays). */
#define SELF_UPDATE_RELEASES_URL SELF_UPDATE_REPO_URL "/releases"
/** Longest versions.txt the board reads. */
#define SELF_UPDATE_VERSIONS_MAX 1024
#define SELF_UPDATE_FETCH_TIMEOUT_MS 15000

/** What versions.txt of a release says. An empty string means the line was absent. */
typedef struct {
    char tag[33];
    char app[32];
    char rcp[32];
} self_update_release_t;

typedef enum {
    SELF_UPDATE_NEWER,   /**< the release is later than the running build */
    SELF_UPDATE_SAME,
    SELF_UPDATE_OLDER,   /**< the running build is later than the release */
    SELF_UPDATE_UNKNOWN, /**< a malformed version, or the same stamp with another hash */
} self_update_cmp_t;

#ifdef __cplusplus
}
#endif
