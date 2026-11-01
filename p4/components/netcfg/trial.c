// Confirm-or-revert (spec 5.13): the trial survives exactly one boot and is committed only by a confirm.
#include "netcfg_trial.h"

#include <stdlib.h>
#include <string.h>

#define MS_PER_S 1000ULL
#define TRIAL_UNTRIED 0
#define TRIAL_TRIED 1

static esp_err_t load_config(const netcfg_env_t *env, const char *key, netcfg_config_t *out)
{
    char text[NETCFG_STORED_MAX];
    esp_err_t err = env->read(env->ctx, key, text, sizeof(text));

    if (err != ESP_OK) {
        return err;
    }
    return netcfg_from_json(text, out, NULL);
}

// The trial goes first: a crash in between leaves a marker without a trial, which the next boot discards harmlessly.
// The other order would leave a trial with no marker, i.e. a trial that would be used a second time.
static void discard_trial(const netcfg_env_t *env)
{
    env->erase(env->ctx, NETCFG_KEY_TRIAL);
    env->erase(env->ctx, NETCFG_KEY_TRIAL_BOOTS);
}

static void load_active(const netcfg_env_t *env, netcfg_config_t *use)
{
    if (load_config(env, NETCFG_KEY_ACTIVE, use) != ESP_OK) {
        netcfg_defaults(use);
    }
}

void netcfg_trial_boot(netcfg_trial_t *t, const netcfg_env_t *env, netcfg_config_t *use)
{
    uint8_t tried = TRIAL_UNTRIED;

    memset(t, 0, sizeof(*t));
    t->env = env;
    const esp_err_t marker = env->read_u8(env->ctx, NETCFG_KEY_TRIAL_BOOTS, &tried);

    if (marker == ESP_ERR_NOT_FOUND) {
        tried = TRIAL_UNTRIED;
    } else if (marker != ESP_OK) {
        tried = TRIAL_TRIED; // unreadable: assume the trial was used already (conservative, it reverts)
    }
    netcfg_config_t trial;
    bool have_trial = load_config(env, NETCFG_KEY_TRIAL, &trial) == ESP_OK;

    if (have_trial && tried == TRIAL_UNTRIED && env->write_u8(env->ctx, NETCFG_KEY_TRIAL_BOOTS, TRIAL_TRIED) == ESP_OK) {
        // Marked as tried before it is used: a reset from here on reverts to the active config.
        t->on_trial = true;
        t->trial = trial;
        t->started_ms = env->now_ms(env->ctx);
        *use = trial;
        return;
    }
    discard_trial(env);
    load_active(env, use);
}

esp_err_t netcfg_trial_apply(const netcfg_env_t *env, const netcfg_config_t *cfg, const char **bad_field)
{
    esp_err_t err = netcfg_validate(cfg, bad_field);

    if (err != ESP_OK) {
        return err;
    }
    char *text = netcfg_to_json(cfg);

    if (!text) {
        return ESP_ERR_NO_MEM;
    }
    // The tried-marker first: a crash between the two writes leaves no trial, never a stale marker on a new trial.
    err = env->write_u8(env->ctx, NETCFG_KEY_TRIAL_BOOTS, TRIAL_UNTRIED);
    if (err == ESP_OK) {
        err = env->write(env->ctx, NETCFG_KEY_TRIAL, text);
    }
    free(text);
    if (err != ESP_OK) {
        discard_trial(env);
        return err;
    }
    env->restart(env->ctx);
    return ESP_OK;
}

esp_err_t netcfg_trial_confirm(netcfg_trial_t *t)
{
    if (!t->on_trial) {
        return ESP_ERR_INVALID_STATE;
    }
    char *text = netcfg_to_json(&t->trial);

    if (!text) {
        return ESP_ERR_NO_MEM;
    }
    // Active first, trial erased after: a reset in between finds a tried trial and discards it, the active config is the same.
    esp_err_t err = t->env->write(t->env->ctx, NETCFG_KEY_ACTIVE, text);

    free(text);
    if (err != ESP_OK) {
        return err;
    }
    discard_trial(t->env);
    t->on_trial = false;
    return ESP_OK;
}

bool netcfg_trial_expired(const netcfg_trial_t *t)
{
    return t->on_trial && t->env->now_ms(t->env->ctx) - t->started_ms >= NETCFG_TRIAL_TIMEOUT_S * MS_PER_S;
}

uint32_t netcfg_trial_remaining_s(const netcfg_trial_t *t)
{
    if (!t->on_trial) {
        return 0;
    }
    const uint64_t elapsed_ms = t->env->now_ms(t->env->ctx) - t->started_ms;
    const uint64_t total_ms = NETCFG_TRIAL_TIMEOUT_S * MS_PER_S;

    return elapsed_ms >= total_ms ? 0 : (uint32_t)((total_ms - elapsed_ms + MS_PER_S - 1) / MS_PER_S);
}

void netcfg_trial_revert(netcfg_trial_t *t)
{
    discard_trial(t->env);
    t->on_trial = false;
    t->env->restart(t->env->ctx);
}

bool netcfg_trial_poll(netcfg_trial_t *t)
{
    if (!netcfg_trial_expired(t)) {
        return false;
    }
    netcfg_trial_revert(t);
    return true;
}

netcfg_apply_action_t netcfg_apply_decide(bool apply_ok, bool on_trial, bool running_is_defaults)
{
    if (apply_ok) {
        return NETCFG_APPLY_DONE;
    }
    if (on_trial) {
        return NETCFG_APPLY_REVERT_TRIAL;
    }
    return running_is_defaults ? NETCFG_APPLY_GIVE_UP : NETCFG_APPLY_TRY_DEFAULTS;
}

esp_err_t netcfg_trial_reset(const netcfg_env_t *env)
{
    esp_err_t err = env->erase(env->ctx, NETCFG_KEY_ACTIVE);

    discard_trial(env);
    if (err == ESP_OK) {
        env->restart(env->ctx);
    }
    return err;
}
