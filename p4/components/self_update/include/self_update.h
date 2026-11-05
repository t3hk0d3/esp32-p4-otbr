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

typedef enum {
    SELF_UPDATE_NEVER,
    SELF_UPDATE_CHECKING,
    SELF_UPDATE_DONE,
    SELF_UPDATE_FAILED,
} self_update_state_t;

/** A consistent copy of the cached check result. */
typedef struct {
    self_update_state_t state;
    bool has_age;  /**< a check has finished (done or failed) */
    uint32_t age_s; /**< seconds since that check */
    char error[160]; /**< detail of the last failed check; "" after a successful one */
    bool has_release; /**< a check succeeded at some point; the release is kept when a later one fails */
    self_update_release_t release;
} self_update_snapshot_t;

/** Move to checking. False when a check is already running (the caller joins it). */
bool self_update_check_begin(void);

/** Fetch and parse versions.txt of the latest release into the cache. Blocking; call only after begin returned true. */
void self_update_check_run(void);

/**
 * Like self_update_check_run, but calls `fetched(ctx)` once the network part is over, right before the result is
 * published (the state still reads checking at that point). `fetched` may be NULL.
 */
void self_update_check_run_then(void (*fetched)(void *ctx), void *ctx);

/**
 * A fresh fetch and parse of the latest release's versions.txt. Does not touch the cache. On failure `detail`
 * (may be NULL) says why.
 */
esp_err_t self_update_fetch_release(self_update_release_t *out, char *detail, size_t cap);

/** Copy of the cached state. Never waits for the network. */
void self_update_get(self_update_snapshot_t *out);

/** Base of the release URLs: SELF_UPDATE_RELEASES_URL unless a test build overrides it. */
const char *self_update_base(void);

#ifdef OTBR_TEST_BUILD
/** Test builds: override the releases base (an http URL); NULL or "" restores the default. */
void self_update_test_set_base(const char *base);
#endif

#ifdef __cplusplus
}
#endif
