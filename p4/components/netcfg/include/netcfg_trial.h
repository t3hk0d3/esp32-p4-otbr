#pragma once

// Confirm-or-revert state machine (spec 5.13): the stored active config, the pending trial and the 5-minute timer.
// All storage and the clock sit behind netcfg_env_t, so the machine is host-tested with a fake store and clock.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "netcfg_config.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NETCFG_TRIAL_TIMEOUT_S 300
/** Store keys (NVS namespace "otbr_net"). */
#define NETCFG_KEY_ACTIVE "active"
#define NETCFG_KEY_TRIAL "trial"
#define NETCFG_KEY_TRIAL_BOOTS "trial_boots"
/** Longest stored config text, terminator included. */
#define NETCFG_STORED_MAX (NETCFG_JSON_MAX + 1)

typedef struct {
    /** Read the text stored under `key` into `buf` (NUL-terminated). ESP_ERR_NOT_FOUND when absent. */
    esp_err_t (*read)(void *ctx, const char *key, char *buf, size_t cap);
    esp_err_t (*write)(void *ctx, const char *key, const char *text);
    esp_err_t (*read_u8)(void *ctx, const char *key, uint8_t *value); /**< ESP_ERR_NOT_FOUND when absent */
    esp_err_t (*write_u8)(void *ctx, const char *key, uint8_t value);
    esp_err_t (*erase)(void *ctx, const char *key); /**< an absent key is ESP_OK */
    uint64_t (*now_ms)(void *ctx);
    void (*restart)(void *ctx); /**< reboot soon (the caller of a request gets its reply first) */
    void *ctx;
} netcfg_env_t;

typedef struct {
    const netcfg_env_t *env;
    bool on_trial;
    netcfg_config_t trial;
    uint64_t started_ms;
} netcfg_trial_t;

/**
 * Decide the configuration of this boot. A stored trial is used once: it is marked as tried (trial_boots) before it is
 * returned, and a trial that was already tried (the device reset before it was confirmed) or does not validate is discarded.
 * Otherwise the stored active config is used, or the defaults when there is none or it does not validate. Never fails:
 * a storage error falls back to the active config / defaults. `t->on_trial` tells which case applies.
 */
void netcfg_trial_boot(netcfg_trial_t *t, const netcfg_env_t *env, netcfg_config_t *use);

/** Validate `cfg` and store it as the pending trial, then ask for a restart. Nothing is stored for an invalid config. */
esp_err_t netcfg_trial_apply(const netcfg_env_t *env, const netcfg_config_t *cfg, const char **bad_field);

/** The trial becomes the active config. ESP_ERR_INVALID_STATE when not on trial; a storage error leaves the trial running. */
esp_err_t netcfg_trial_confirm(netcfg_trial_t *t);

/** True when the trial is running and its time is up. */
bool netcfg_trial_expired(const netcfg_trial_t *t);

/** Seconds left of the trial, 0 when none. */
uint32_t netcfg_trial_remaining_s(const netcfg_trial_t *t);

/** When the trial is expired: discard it and ask for a restart into the previous active config. Returns true if it did. */
bool netcfg_trial_poll(netcfg_trial_t *t);

/** Discard the running trial at once (it could not be applied) and ask for a restart into the previous active config. */
void netcfg_trial_revert(netcfg_trial_t *t);

typedef enum {
    NETCFG_APPLY_DONE,         /**< the interfaces are up as configured */
    NETCFG_APPLY_TRY_DEFAULTS, /**< the active config cannot be applied: bring up the defaults in this boot */
    NETCFG_APPLY_REVERT_TRIAL, /**< the trial cannot be applied: discard it and reboot into the previous config */
    NETCFG_APPLY_GIVE_UP,      /**< not even the defaults came up: keep the rest of the boot (HTTP, console) alive */
} netcfg_apply_action_t;

/** What to do with the result of bringing up the interfaces (`running_is_defaults`: the config was the defaults, retried or not). */
netcfg_apply_action_t netcfg_apply_decide(bool apply_ok, bool on_trial, bool running_is_defaults);

/**
 * Store `enabled` as the TREL setting of the stored active config (the defaults when none is stored or it does not
 * validate), without a trial or a restart. ESP_ERR_INVALID_STATE while a trial runs (confirm or revert it first); no
 * write when the stored value is already `enabled`; a storage error leaves the stored config as it was.
 */
esp_err_t netcfg_trial_set_trel(const netcfg_trial_t *t, bool enabled);

/** Erase the active config and any trial, then ask for a restart: the next boot uses the defaults. */
esp_err_t netcfg_trial_reset(const netcfg_env_t *env);

#ifdef __cplusplus
}
#endif
