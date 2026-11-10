// Settings of the logging component: load from NVS, validate, put in force (ESP-IDF log levels, level listeners) and
// store. One mutex serialises everything that reads or changes the settings; listeners are called after it is
// released, under a second mutex that keeps their calls in order.

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "logging_core.h"
#include "logging_priv.h"
#if !CONFIG_IDF_TARGET_LINUX
#include "nvs.h"
#endif

static const char *TAG = "logging";

typedef struct {
    char tag[LOGGING_TAG_MAX];
    logging_level_cb_t cb;
    esp_log_level_t delivered; /**< Last level the listener got (or the level in force when it was added). */
} listener_t;

typedef struct {
    logging_level_cb_t cb;
    esp_log_level_t level;
} pending_call_t;

#if !CONFIG_IDF_TARGET_LINUX

static esp_err_t nvs_blob_get(const char *ns, const char *key, void *buf, size_t *len)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(ns, NVS_READONLY, &h);

    if (err == ESP_OK) {
        err = nvs_get_blob(h, key, buf, len);
        nvs_close(h);
    }
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_ERR_NOT_FOUND;
    }
    return err == ESP_ERR_NVS_INVALID_LENGTH ? ESP_ERR_INVALID_SIZE : err;
}

static esp_err_t nvs_blob_set(const char *ns, const char *key, const void *buf, size_t len)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(ns, NVS_READWRITE, &h);

    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(h, key, buf, len);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static const logging_ops_t TARGET_OPS = {
    .blob_get = nvs_blob_get,
    .blob_set = nvs_blob_set,
    .level_set = esp_log_level_set,
};

static const logging_ops_t *s_ops = &TARGET_OPS;
#else
static const logging_ops_t *s_ops; // host tests set fakes
#endif

static SemaphoreHandle_t s_lock;        // s_settings, s_viewers_max, s_listeners, s_blob, s_bad_field
static SemaphoreHandle_t s_notify_lock; // one listener delivery at a time; taken without s_lock held
static logging_settings_t s_settings;
static esp_log_level_t s_viewers_max;
static listener_t s_listeners[LOGGING_LISTENERS_MAX];
static size_t s_n_listeners;
static uint8_t s_blob[LOGGING_BLOB_SIZE];
static char s_bad_field[sizeof("components.") + LOGGING_TAG_MAX];
// Read by the capture without any lock (it must never take s_lock); written under s_lock (or before it exists).
// The highest level compiled in: Info in a release build, Verbose in a test build (p4/sdkconfig.defaults.test). Host
// tests start at Verbose and lower it with logging_set_ceiling.
#if CONFIG_IDF_TARGET_LINUX
#define CEILING_BUILD ESP_LOG_VERBOSE
#else
#define CEILING_BUILD (CONFIG_LOG_MAXIMUM_LEVEL < ESP_LOG_VERBOSE ? (esp_log_level_t) CONFIG_LOG_MAXIMUM_LEVEL : ESP_LOG_VERBOSE)
#endif
static esp_log_level_t s_ceiling = CEILING_BUILD;
uint32_t logging_levels_word;  // logging_levels_t.word (logging_priv.h)
bool logging_levels_published; // set with the first publish (logging_priv.h)
static uint32_t s_generation;

void logging_set_ops(const logging_ops_t *ops)
{
    s_ops = ops;
}

void logging_set_ceiling(esp_log_level_t ceiling)
{
    s_ceiling = ceiling;
}

esp_log_level_t logging_level_ceiling(void)
{
    return s_ceiling;
}

void logging_reset(void)
{
    s_ceiling = CEILING_BUILD;
    if (s_lock) {
        vSemaphoreDelete(s_lock);
        vSemaphoreDelete(s_notify_lock);
    }
    s_lock = NULL;
    s_notify_lock = NULL;
    memset(&s_settings, 0, sizeof(s_settings));
    s_viewers_max = ESP_LOG_NONE;
    memset(s_listeners, 0, sizeof(s_listeners));
    s_n_listeners = 0;
    __atomic_store_n(&logging_levels_word, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&logging_levels_published, false, __ATOMIC_RELEASE);
    __atomic_store_n(&s_generation, 0, __ATOMIC_RELEASE);
}

/**
 * Hand every gate to ESP-IDF: the default first (ESP-IDF's "*" also forgets every per-tag level), then each override.
 * Between the two, a line from an override tag on another task is briefly filtered at the default's gate. Then publish
 * the channel levels for the capture. Under s_lock (or before it exists).
 */
static void set_levels(void)
{
    logging_levels_t lv = logging_core_levels(&s_settings, s_viewers_max);

    s_ops->level_set("*", logging_core_gate(&s_settings, "*", s_viewers_max));
    for (int i = 0; i < s_settings.n_components; i++) {
        const char *tag = s_settings.components[i].tag;

        s_ops->level_set(tag, logging_core_gate(&s_settings, tag, s_viewers_max));
    }
    __atomic_store_n(&logging_levels_word, lv.word, __ATOMIC_RELEASE);
    __atomic_store_n(&logging_levels_published, true, __ATOMIC_RELEASE);
}

/**
 * Tell each listener whose effective level differs from what it last got. Called without s_lock held; the levels are
 * read afresh under s_notify_lock, so concurrent changes reach the listeners in order and the last call carries the
 * level in force.
 */
static void notify_listeners(void)
{
    pending_call_t calls[LOGGING_LISTENERS_MAX];
    size_t n = 0;

    xSemaphoreTake(s_notify_lock, portMAX_DELAY);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (size_t i = 0; i < s_n_listeners; i++) {
        listener_t *l = &s_listeners[i];
        esp_log_level_t level = logging_core_gate(&s_settings, l->tag, s_viewers_max);

        if (level != l->delivered) {
            l->delivered = level;
            calls[n++] = (pending_call_t) {
                .cb = l->cb, .level = level
            };
        }
    }
    xSemaphoreGive(s_lock);
    for (size_t i = 0; i < n; i++) {
        calls[i].cb(calls[i].level);
    }
    xSemaphoreGive(s_notify_lock);
}

/**
 * Load the stored settings into s_settings, or leave the defaults there with one warning when they are unusable.
 * Runs before the locks exist, so the warning cannot meet a lock of this component.
 */
static void load(void)
{
    size_t len = sizeof(s_blob);
    esp_err_t err = s_ops->blob_get(LOGGING_NVS_NAMESPACE, LOGGING_NVS_KEY, s_blob, &len);
    const char *field = NULL;

    logging_core_defaults(&s_settings);
    if (err == ESP_ERR_NOT_FOUND) {
        return;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "stored settings unreadable (%s), using defaults", esp_err_to_name(err));
        return;
    }
    if (len != LOGGING_BLOB_SIZE || s_blob[0] != LOGGING_BLOB_VERSION) {
        ESP_LOGW(TAG, "stored settings are version %u, %u bytes (want %u, %u), using defaults", (unsigned) s_blob[0],
                 (unsigned) len, LOGGING_BLOB_VERSION, (unsigned) LOGGING_BLOB_SIZE);
        return;
    }
    memcpy(&s_settings, &s_blob[1], sizeof(s_settings));
    if (logging_core_validate(&s_settings, &field) != ESP_OK) {
        ESP_LOGW(TAG, "stored settings invalid at %s, using defaults", field);
        logging_core_defaults(&s_settings);
        return;
    }
    // Stored by a test build: this build compiles nothing above the ceiling, so those levels become the ceiling (in
    // force only; NVS keeps them until the next apply).
    if (logging_core_clamp(&s_settings, s_ceiling)) {
        ESP_LOGW(TAG, "stored levels above %s lowered to %s (this build has no lower levels)",
                 logging_level_name(s_ceiling), logging_level_name(s_ceiling));
    }
}

esp_err_t logging_init(void)
{
    if (s_lock) {
        return ESP_OK;
    }
    if (!s_ops) {
        return ESP_ERR_INVALID_STATE;
    }
    s_viewers_max = ESP_LOG_NONE;
    load();
    set_levels();
    s_notify_lock = xSemaphoreCreateMutex();
    if (!s_notify_lock) {
        return ESP_ERR_NO_MEM;
    }
    s_lock = xSemaphoreCreateMutex(); // last: a lock means initialised
    if (!s_lock) {
        vSemaphoreDelete(s_notify_lock);
        s_notify_lock = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool logging_is_initialised(void)
{
    return s_lock != NULL;
}

esp_err_t logging_get_settings(logging_settings_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(out, &s_settings, sizeof(*out));
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t logging_apply(const logging_settings_t *settings, const char **bad_field)
{
    const char *field = NULL;

    if (!settings) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (logging_core_validate(settings, &field) != ESP_OK || logging_core_above(settings, s_ceiling, &field) != ESP_OK) {
        strlcpy(s_bad_field, field ? field : "", sizeof(s_bad_field));
        if (bad_field) {
            *bad_field = s_bad_field;
        }
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_ARG;
    }
    // A removed override needs no call of its own: set_levels() starts with "*", which in ESP-IDF v6.1 forgets every
    // per-tag level, so the tag follows the default again.
    memcpy(&s_settings, settings, sizeof(s_settings));
    set_levels();
    __atomic_add_fetch(&s_generation, 1, __ATOMIC_RELEASE);

    s_blob[0] = LOGGING_BLOB_VERSION;
    memcpy(&s_blob[1], &s_settings, sizeof(s_settings));
    esp_err_t stored = s_ops->blob_set(LOGGING_NVS_NAMESPACE, LOGGING_NVS_KEY, s_blob, sizeof(s_blob));

    xSemaphoreGive(s_lock);
    notify_listeners();
    return stored == ESP_OK ? ESP_OK : ESP_FAIL;
}

esp_err_t logging_level_listener_add(const char *tag, logging_level_cb_t cb)
{
    esp_log_level_t level;

    if (!tag || !cb || tag[0] == '\0' || strlen(tag) >= LOGGING_TAG_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    // Under s_notify_lock, so a concurrent change cannot deliver a newer level before this first call.
    xSemaphoreTake(s_notify_lock, portMAX_DELAY);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_n_listeners >= LOGGING_LISTENERS_MAX) {
        xSemaphoreGive(s_lock);
        xSemaphoreGive(s_notify_lock);
        return ESP_ERR_NO_MEM;
    }
    listener_t *l = &s_listeners[s_n_listeners++];

    strlcpy(l->tag, tag, sizeof(l->tag));
    l->cb = cb;
    level = logging_core_gate(&s_settings, tag, s_viewers_max);
    l->delivered = level;
    xSemaphoreGive(s_lock);
    cb(level);
    xSemaphoreGive(s_notify_lock);
    return ESP_OK;
}

void logging_viewers_max_changed(esp_log_level_t viewers_max)
{
    if (!s_lock) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_viewers_max = viewers_max;
    set_levels();
    xSemaphoreGive(s_lock);
    notify_listeners();
}

logging_levels_t logging_levels(void)
{
    logging_levels_t lv = {.word = __atomic_load_n(&logging_levels_word, __ATOMIC_ACQUIRE)};

    return lv;
}

uint32_t logging_settings_generation(void)
{
    return __atomic_load_n(&s_generation, __ATOMIC_ACQUIRE);
}
