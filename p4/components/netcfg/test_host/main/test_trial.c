#include <string.h>
#include "unity.h"
#include "unity_test_runner.h"
#include "netcfg_trial.h"

#define FAKE_KEYS 4
#define MINUTE_MS (60ULL * 1000)

// Fake NVS, clock and restart.
typedef struct {
    struct {
        char key[16];
        char text[NETCFG_STORED_MAX];
        uint8_t u8;
        bool is_u8;
        bool used;
    } slot[FAKE_KEYS];
    uint64_t now_ms;
    int restarts;
    int writes;
    bool fail_writes;
    bool fail_read_u8;
} fake_t;

static fake_t s_fake;

static __typeof__(s_fake.slot[0]) *find(const char *key, bool create)
{
    for (int i = 0; i < FAKE_KEYS; i++) {
        if (s_fake.slot[i].used && strcmp(s_fake.slot[i].key, key) == 0) {
            return &s_fake.slot[i];
        }
    }
    for (int i = 0; create && i < FAKE_KEYS; i++) {
        if (!s_fake.slot[i].used) {
            memset(&s_fake.slot[i], 0, sizeof(s_fake.slot[i]));
            strcpy(s_fake.slot[i].key, key);
            s_fake.slot[i].used = true;
            return &s_fake.slot[i];
        }
    }
    return NULL;
}

static esp_err_t fake_read(void *ctx, const char *key, char *buf, size_t cap)
{
    __typeof__(s_fake.slot[0]) *slot = find(key, false);

    if (!slot || slot->is_u8) {
        return ESP_ERR_NOT_FOUND;
    }
    strlcpy(buf, slot->text, cap);
    return ESP_OK;
}

static esp_err_t fake_write(void *ctx, const char *key, const char *text)
{
    if (s_fake.fail_writes) {
        return ESP_FAIL;
    }
    s_fake.writes++;
    strlcpy(find(key, true)->text, text, NETCFG_STORED_MAX);
    return ESP_OK;
}

static esp_err_t fake_read_u8(void *ctx, const char *key, uint8_t *value)
{
    __typeof__(s_fake.slot[0]) *slot = find(key, false);

    if (s_fake.fail_read_u8) {
        return ESP_FAIL;
    }
    if (!slot || !slot->is_u8) {
        return ESP_ERR_NOT_FOUND;
    }
    *value = slot->u8;
    return ESP_OK;
}

static esp_err_t fake_write_u8(void *ctx, const char *key, uint8_t value)
{
    if (s_fake.fail_writes) {
        return ESP_FAIL;
    }
    s_fake.writes++;
    __typeof__(s_fake.slot[0]) *slot = find(key, true);

    slot->u8 = value;
    slot->is_u8 = true;
    return ESP_OK;
}

static esp_err_t fake_erase(void *ctx, const char *key)
{
    __typeof__(s_fake.slot[0]) *slot = find(key, false);

    if (slot) {
        slot->used = false;
    }
    return ESP_OK;
}

static uint64_t fake_now(void *ctx)
{
    return s_fake.now_ms;
}

static void fake_restart(void *ctx)
{
    s_fake.restarts++;
}

static const netcfg_env_t s_env = {
    .read = fake_read, .write = fake_write, .read_u8 = fake_read_u8, .write_u8 = fake_write_u8,
    .erase = fake_erase, .now_ms = fake_now, .restart = fake_restart,
};

static netcfg_config_t vlan_single(uint16_t vlan)
{
    netcfg_config_t cfg;

    netcfg_defaults(&cfg);
    cfg.single.vlan = vlan;
    return cfg;
}

static netcfg_config_t split(uint16_t mgmt_vlan, uint16_t bb_vlan)
{
    netcfg_config_t cfg;

    netcfg_defaults(&cfg);
    cfg.mode = NETCFG_MODE_SPLIT;
    cfg.mgmt.vlan = mgmt_vlan;
    cfg.thread_bb.vlan = bb_vlan;
    return cfg;
}

// Store `active` as the confirmed config through a full apply, boot, confirm cycle.
static void store_active(const netcfg_config_t *cfg)
{
    netcfg_trial_t t;
    netcfg_config_t use;

    TEST_ASSERT_EQUAL(ESP_OK, netcfg_trial_apply(&s_env, cfg, NULL));
    netcfg_trial_boot(&t, &s_env, &use);
    TEST_ASSERT_TRUE(t.on_trial);
    TEST_ASSERT_EQUAL(ESP_OK, netcfg_trial_confirm(&t));
    s_fake.restarts = 0;
}

static void reset_fake(void)
{
    memset(&s_fake, 0, sizeof(s_fake));
}

TEST_CASE("On a boot with nothing stored: the defaults are used and no trial runs", "[netcfg]")
{
    netcfg_trial_t t;
    netcfg_config_t use, defaults;

    reset_fake();
    netcfg_defaults(&defaults);
    netcfg_trial_boot(&t, &s_env, &use);
    TEST_ASSERT_FALSE(t.on_trial);
    TEST_ASSERT_TRUE(netcfg_equal(&defaults, &use));
    TEST_ASSERT_EQUAL(0, netcfg_trial_remaining_s(&t));
}

TEST_CASE("On a boot with a stored active config and no trial: the active config is used", "[netcfg]")
{
    netcfg_trial_t t;
    netcfg_config_t use, active = vlan_single(30);

    reset_fake();
    store_active(&active);
    netcfg_trial_boot(&t, &s_env, &use);
    TEST_ASSERT_FALSE(t.on_trial);
    TEST_ASSERT_TRUE(netcfg_equal(&active, &use));
}

TEST_CASE("On a boot with a pending trial: the trial is used, marked tried, and 300 s remain", "[netcfg]")
{
    netcfg_trial_t t;
    netcfg_config_t use, active = vlan_single(30), trial = split(10, 20);
    uint8_t tried = 0xff;

    reset_fake();
    store_active(&active);
    TEST_ASSERT_EQUAL(ESP_OK, netcfg_trial_apply(&s_env, &trial, NULL));
    s_fake.now_ms = 5000;
    netcfg_trial_boot(&t, &s_env, &use);
    TEST_ASSERT_TRUE(t.on_trial);
    TEST_ASSERT_TRUE(netcfg_equal(&trial, &use));
    TEST_ASSERT_EQUAL(ESP_OK, fake_read_u8(NULL, NETCFG_KEY_TRIAL_BOOTS, &tried));
    TEST_ASSERT_EQUAL(1, tried);
    TEST_ASSERT_EQUAL(NETCFG_TRIAL_TIMEOUT_S, netcfg_trial_remaining_s(&t));
    s_fake.now_ms += 100 * 1000;
    TEST_ASSERT_EQUAL(200, netcfg_trial_remaining_s(&t));
}

TEST_CASE("On PUT with a valid config: it is stored as pending trial and a restart is requested", "[netcfg]")
{
    netcfg_config_t trial = split(10, 20), stored, active = vlan_single(30);
    char text[NETCFG_STORED_MAX];
    uint8_t tried = 0xff;

    reset_fake();
    store_active(&active);
    TEST_ASSERT_EQUAL(ESP_OK, netcfg_trial_apply(&s_env, &trial, NULL));
    TEST_ASSERT_EQUAL(1, s_fake.restarts);
    TEST_ASSERT_EQUAL(ESP_OK, fake_read(NULL, NETCFG_KEY_TRIAL, text, sizeof(text)));
    TEST_ASSERT_EQUAL(ESP_OK, netcfg_from_json(text, &stored, NULL));
    TEST_ASSERT_TRUE(netcfg_equal(&trial, &stored));
    TEST_ASSERT_EQUAL(ESP_OK, fake_read_u8(NULL, NETCFG_KEY_TRIAL_BOOTS, &tried));
    TEST_ASSERT_EQUAL(0, tried);
    // the active config is untouched
    TEST_ASSERT_EQUAL(ESP_OK, fake_read(NULL, NETCFG_KEY_ACTIVE, text, sizeof(text)));
    TEST_ASSERT_EQUAL(ESP_OK, netcfg_from_json(text, &stored, NULL));
    TEST_ASSERT_TRUE(netcfg_equal(&active, &stored));
}

TEST_CASE("On PUT with an invalid config: it names the field, stores nothing and does not restart", "[netcfg]")
{
    netcfg_config_t bad = split(20, 20);
    const char *field = NULL;

    reset_fake();
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, netcfg_trial_apply(&s_env, &bad, &field));
    TEST_ASSERT_EQUAL_STRING("vlan", field);
    TEST_ASSERT_EQUAL(0, s_fake.writes);
    TEST_ASSERT_EQUAL(0, s_fake.restarts);
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, fake_read(NULL, NETCFG_KEY_TRIAL, (char[8]) {
        0
    }, 8));
}

TEST_CASE("On PUT when storing fails: the error is returned, no trial is left and no restart happens", "[netcfg]")
{
    netcfg_config_t trial = split(10, 20);

    reset_fake();
    s_fake.fail_writes = true;
    TEST_ASSERT_EQUAL(ESP_FAIL, netcfg_trial_apply(&s_env, &trial, NULL));
    TEST_ASSERT_EQUAL(0, s_fake.restarts);
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, fake_read(NULL, NETCFG_KEY_TRIAL, (char[8]) {
        0
    }, 8));
}

TEST_CASE("On a confirm within 5 minutes: the trial becomes the active config and the trial is erased", "[netcfg]")
{
    netcfg_trial_t t;
    netcfg_config_t use, trial = split(10, 20), next;

    reset_fake();
    TEST_ASSERT_EQUAL(ESP_OK, netcfg_trial_apply(&s_env, &trial, NULL));
    s_fake.restarts = 0;
    netcfg_trial_boot(&t, &s_env, &use);
    s_fake.now_ms += 4 * MINUTE_MS + 59 * 1000;
    TEST_ASSERT_FALSE(netcfg_trial_poll(&t));
    TEST_ASSERT_EQUAL(ESP_OK, netcfg_trial_confirm(&t));
    TEST_ASSERT_FALSE(t.on_trial);
    TEST_ASSERT_EQUAL(0, s_fake.restarts);
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, fake_read(NULL, NETCFG_KEY_TRIAL, (char[8]) {
        0
    }, 8));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, fake_read_u8(NULL, NETCFG_KEY_TRIAL_BOOTS, &(uint8_t) {
        0
    }));
    // later boot: the confirmed config is active, no trial
    netcfg_trial_boot(&t, &s_env, &next);
    TEST_ASSERT_FALSE(t.on_trial);
    TEST_ASSERT_TRUE(netcfg_equal(&trial, &next));
}

TEST_CASE("On a confirm without a trial: INVALID_STATE", "[netcfg]")
{
    netcfg_trial_t t;
    netcfg_config_t use;

    reset_fake();
    netcfg_trial_boot(&t, &s_env, &use);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, netcfg_trial_confirm(&t));
}

TEST_CASE("On a confirm when storing the active config fails: the error is returned and the trial keeps running", "[netcfg]")
{
    netcfg_trial_t t;
    netcfg_config_t use, trial = split(10, 20);

    reset_fake();
    TEST_ASSERT_EQUAL(ESP_OK, netcfg_trial_apply(&s_env, &trial, NULL));
    netcfg_trial_boot(&t, &s_env, &use);
    s_fake.fail_writes = true;
    TEST_ASSERT_EQUAL(ESP_FAIL, netcfg_trial_confirm(&t));
    TEST_ASSERT_TRUE(t.on_trial);
}

TEST_CASE("On no confirm within 5 minutes: the trial is discarded, a restart is requested and the next boot uses the previous config", "[netcfg]")
{
    netcfg_trial_t t;
    netcfg_config_t use, active = vlan_single(30), trial = split(10, 20);

    reset_fake();
    store_active(&active);
    TEST_ASSERT_EQUAL(ESP_OK, netcfg_trial_apply(&s_env, &trial, NULL));
    s_fake.restarts = 0;
    netcfg_trial_boot(&t, &s_env, &use);
    s_fake.now_ms += NETCFG_TRIAL_TIMEOUT_S * 1000ULL - 1;
    TEST_ASSERT_FALSE(netcfg_trial_poll(&t));
    TEST_ASSERT_EQUAL(0, s_fake.restarts);
    s_fake.now_ms += 1;
    TEST_ASSERT_TRUE(netcfg_trial_poll(&t));
    TEST_ASSERT_EQUAL(1, s_fake.restarts);
    TEST_ASSERT_FALSE(netcfg_trial_poll(&t)); // once only
    TEST_ASSERT_EQUAL(1, s_fake.restarts);
    netcfg_trial_boot(&t, &s_env, &use);
    TEST_ASSERT_FALSE(t.on_trial);
    TEST_ASSERT_TRUE(netcfg_equal(&active, &use));
}

TEST_CASE("On a reset during the trial: the next boot uses the previous active config", "[netcfg]")
{
    netcfg_trial_t t;
    netcfg_config_t use, active = vlan_single(30), trial = split(10, 20);

    reset_fake();
    store_active(&active);
    TEST_ASSERT_EQUAL(ESP_OK, netcfg_trial_apply(&s_env, &trial, NULL));
    netcfg_trial_boot(&t, &s_env, &use); // the trial boot
    TEST_ASSERT_TRUE(t.on_trial);
    netcfg_trial_boot(&t, &s_env, &use); // reset before any confirm: the stored state is all that survives
    TEST_ASSERT_FALSE(t.on_trial);
    TEST_ASSERT_TRUE(netcfg_equal(&active, &use));
    netcfg_trial_boot(&t, &s_env, &use); // and it stays reverted
    TEST_ASSERT_FALSE(t.on_trial);
    TEST_ASSERT_TRUE(netcfg_equal(&active, &use));
}

TEST_CASE("On a stored trial that does not validate: it is discarded and the active config is used", "[netcfg]")
{
    netcfg_trial_t t;
    netcfg_config_t use, active = vlan_single(30);

    reset_fake();
    store_active(&active);
    fake_write(NULL, NETCFG_KEY_TRIAL, "{\"mode\":\"split\"}");
    fake_write_u8(NULL, NETCFG_KEY_TRIAL_BOOTS, 0);
    netcfg_trial_boot(&t, &s_env, &use);
    TEST_ASSERT_FALSE(t.on_trial);
    TEST_ASSERT_TRUE(netcfg_equal(&active, &use));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, fake_read(NULL, NETCFG_KEY_TRIAL, (char[8]) {
        0
    }, 8));
}

TEST_CASE("On a trial whose tried-marker cannot be written: the trial is not used", "[netcfg]")
{
    netcfg_trial_t t;
    netcfg_config_t use, active = vlan_single(30), trial = split(10, 20);

    reset_fake();
    store_active(&active);
    TEST_ASSERT_EQUAL(ESP_OK, netcfg_trial_apply(&s_env, &trial, NULL));
    s_fake.fail_writes = true;
    netcfg_trial_boot(&t, &s_env, &use);
    TEST_ASSERT_FALSE(t.on_trial);
    TEST_ASSERT_TRUE(netcfg_equal(&active, &use));
}

TEST_CASE("On console net reset: stored configs are erased, a restart is requested and the next boot uses the defaults", "[netcfg]")
{
    netcfg_trial_t t;
    netcfg_config_t use, defaults, active = vlan_single(30), trial = split(10, 20);

    reset_fake();
    netcfg_defaults(&defaults);
    store_active(&active);
    TEST_ASSERT_EQUAL(ESP_OK, netcfg_trial_apply(&s_env, &trial, NULL));
    s_fake.restarts = 0;
    TEST_ASSERT_EQUAL(ESP_OK, netcfg_trial_reset(&s_env));
    TEST_ASSERT_EQUAL(1, s_fake.restarts);
    netcfg_trial_boot(&t, &s_env, &use);
    TEST_ASSERT_FALSE(t.on_trial);
    TEST_ASSERT_TRUE(netcfg_equal(&defaults, &use));
}

TEST_CASE("On a trial whose tried-marker cannot be read: the trial is not used", "[netcfg]")
{
    netcfg_trial_t t;
    netcfg_config_t use, active = vlan_single(30), trial = split(10, 20);

    reset_fake();
    store_active(&active);
    TEST_ASSERT_EQUAL(ESP_OK, netcfg_trial_apply(&s_env, &trial, NULL));
    s_fake.fail_read_u8 = true;
    netcfg_trial_boot(&t, &s_env, &use);
    TEST_ASSERT_FALSE(t.on_trial);
    TEST_ASSERT_TRUE(netcfg_equal(&active, &use));
}

TEST_CASE("On a trial that cannot be applied: it is discarded at once and a restart is requested", "[netcfg]")
{
    netcfg_trial_t t;
    netcfg_config_t use, active = vlan_single(30), trial = split(10, 20);

    reset_fake();
    store_active(&active);
    TEST_ASSERT_EQUAL(ESP_OK, netcfg_trial_apply(&s_env, &trial, NULL));
    s_fake.restarts = 0;
    netcfg_trial_boot(&t, &s_env, &use);
    netcfg_trial_revert(&t);
    TEST_ASSERT_FALSE(t.on_trial);
    TEST_ASSERT_EQUAL(1, s_fake.restarts);
    netcfg_trial_boot(&t, &s_env, &use);
    TEST_ASSERT_FALSE(t.on_trial);
    TEST_ASSERT_TRUE(netcfg_equal(&active, &use));
}

TEST_CASE("On interfaces that came up: nothing else happens", "[netcfg]")
{
    TEST_ASSERT_EQUAL(NETCFG_APPLY_DONE, netcfg_apply_decide(true, false, false));
    TEST_ASSERT_EQUAL(NETCFG_APPLY_DONE, netcfg_apply_decide(true, true, false));
}

TEST_CASE("On an active config that cannot be applied: the defaults are tried in the same boot", "[netcfg]")
{
    TEST_ASSERT_EQUAL(NETCFG_APPLY_TRY_DEFAULTS, netcfg_apply_decide(false, false, false));
}

TEST_CASE("On a trial that cannot be applied: it is reverted, not replaced by the defaults", "[netcfg]")
{
    TEST_ASSERT_EQUAL(NETCFG_APPLY_REVERT_TRIAL, netcfg_apply_decide(false, true, false));
}

TEST_CASE("On defaults that cannot be applied either: give up and keep the rest of the boot alive", "[netcfg]")
{
    TEST_ASSERT_EQUAL(NETCFG_APPLY_GIVE_UP, netcfg_apply_decide(false, false, true));
}

TEST_CASE("On set_trel with nothing stored: the defaults with the new value become the active config, no restart", "[netcfg]")
{
    netcfg_trial_t t;
    netcfg_config_t use, stored;
    char text[NETCFG_STORED_MAX];

    reset_fake();
    netcfg_trial_boot(&t, &s_env, &use);
    TEST_ASSERT_EQUAL(ESP_OK, netcfg_trial_set_trel(&t, false));
    TEST_ASSERT_EQUAL(ESP_OK, fake_read(NULL, NETCFG_KEY_ACTIVE, text, sizeof(text)));
    TEST_ASSERT_EQUAL(ESP_OK, netcfg_from_json(text, &stored, NULL));
    TEST_ASSERT_FALSE(stored.trel);
    TEST_ASSERT_EQUAL(NETCFG_MODE_SINGLE, stored.mode);
    TEST_ASSERT_EQUAL(0, s_fake.restarts);
}

TEST_CASE("On set_trel with a stored split config: only trel changes", "[netcfg]")
{
    netcfg_trial_t t;
    netcfg_config_t use, stored;
    char text[NETCFG_STORED_MAX];
    const netcfg_config_t cfg = split(10, 20);

    reset_fake();
    store_active(&cfg);
    netcfg_trial_boot(&t, &s_env, &use);
    TEST_ASSERT_EQUAL(ESP_OK, netcfg_trial_set_trel(&t, false));
    TEST_ASSERT_EQUAL(ESP_OK, fake_read(NULL, NETCFG_KEY_ACTIVE, text, sizeof(text)));
    TEST_ASSERT_EQUAL(ESP_OK, netcfg_from_json(text, &stored, NULL));
    TEST_ASSERT_TRUE(netcfg_diff_only_trel(&cfg, &stored));
    TEST_ASSERT_EQUAL(0, s_fake.restarts);
}

TEST_CASE("On set_trel with the value already stored: nothing is written", "[netcfg]")
{
    netcfg_trial_t t;
    netcfg_config_t use;
    const netcfg_config_t cfg = vlan_single(5);

    reset_fake();
    store_active(&cfg);
    netcfg_trial_boot(&t, &s_env, &use);
    s_fake.writes = 0;
    TEST_ASSERT_EQUAL(ESP_OK, netcfg_trial_set_trel(&t, true));
    TEST_ASSERT_EQUAL(0, s_fake.writes);
}

TEST_CASE("On set_trel while a trial runs: refused, nothing is written", "[netcfg]")
{
    netcfg_trial_t t;
    netcfg_config_t use;
    const netcfg_config_t cfg = vlan_single(5);

    reset_fake();
    TEST_ASSERT_EQUAL(ESP_OK, netcfg_trial_apply(&s_env, &cfg, NULL));
    netcfg_trial_boot(&t, &s_env, &use);
    TEST_ASSERT_TRUE(t.on_trial);
    s_fake.writes = 0;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, netcfg_trial_set_trel(&t, false));
    TEST_ASSERT_EQUAL(0, s_fake.writes);
}

TEST_CASE("On set_trel with a storage error: the error is returned, the stored config stays", "[netcfg]")
{
    netcfg_trial_t t;
    netcfg_config_t use, stored;
    char text[NETCFG_STORED_MAX];
    const netcfg_config_t cfg = vlan_single(5);

    reset_fake();
    store_active(&cfg);
    netcfg_trial_boot(&t, &s_env, &use);
    s_fake.fail_writes = true;
    TEST_ASSERT_NOT_EQUAL(ESP_OK, netcfg_trial_set_trel(&t, false));
    TEST_ASSERT_EQUAL(ESP_OK, fake_read(NULL, NETCFG_KEY_ACTIVE, text, sizeof(text)));
    TEST_ASSERT_EQUAL(ESP_OK, netcfg_from_json(text, &stored, NULL));
    TEST_ASSERT_TRUE(stored.trel);
}
