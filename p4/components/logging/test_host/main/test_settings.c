#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "unity.h"
#include "logging.h"
#include "logging_core.h"
#include "logging_priv.h"

/* ---- Fake NVS and esp_log_level_set ---- */

#define LEVELS_MAX 48

static struct {
    uint8_t blob[LOGGING_BLOB_SIZE + 16];
    size_t blob_len; /**< 0 = no blob. */
    char ns[16];
    char key[16];
    int sets;
    esp_err_t set_result;
    struct {
        char tag[LOGGING_TAG_MAX];
        esp_log_level_t level;
    } levels[LEVELS_MAX];
    int n_levels;
    int level_calls;
    const logging_settings_t *inputs; /**< When set, every write must leave one of inputs[0..1] in the blob. */
    int mixed;                        /**< Writes that left anything else. */
    int writing;                      /**< Writes in progress. */
    int overlaps;                     /**< Writes that started while another was in progress. */
} s_fake;

static esp_err_t fake_blob_get(const char *ns, const char *key, void *buf, size_t *len)
{
    if (s_fake.blob_len == 0 || strcmp(ns, s_fake.ns) != 0 || strcmp(key, s_fake.key) != 0) {
        return ESP_ERR_NOT_FOUND;
    }
    if (s_fake.blob_len > *len) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(buf, s_fake.blob, s_fake.blob_len);
    *len = s_fake.blob_len;
    return ESP_OK;
}

static esp_err_t fake_blob_set(const char *ns, const char *key, const void *buf, size_t len)
{
    const uint8_t *src = buf;

    s_fake.sets++;
    if (s_fake.set_result != ESP_OK) {
        return s_fake.set_result;
    }
    if (s_fake.writing++ > 0) {
        s_fake.overlaps++;
    }
    strlcpy(s_fake.ns, ns, sizeof(s_fake.ns));
    strlcpy(s_fake.key, key, sizeof(s_fake.key));
    // Byte by byte with a yield halfway: two unserialised writers overlap here and may leave a mix.
    for (size_t i = 0; i < len && i < sizeof(s_fake.blob); i++) {
        s_fake.blob[i] = src[i];
        if (i == len / 2) {
            taskYIELD();
        }
    }
    s_fake.blob_len = len;
    s_fake.writing--;
    if (s_fake.inputs && memcmp(s_fake.blob + 1, &s_fake.inputs[0], sizeof(logging_settings_t)) != 0 &&
            memcmp(s_fake.blob + 1, &s_fake.inputs[1], sizeof(logging_settings_t)) != 0) {
        s_fake.mixed++;
    }
    return ESP_OK;
}

/** Like ESP-IDF v6.1's esp_log_level_set: "*" sets the default and forgets every per-tag level. */
static void fake_level_set(const char *tag, esp_log_level_t level)
{
    s_fake.level_calls++;
    if (strcmp(tag, "*") == 0) {
        s_fake.n_levels = 0;
    }
    for (int i = 0; i < s_fake.n_levels; i++) {
        if (strcmp(s_fake.levels[i].tag, tag) == 0) {
            s_fake.levels[i].level = level;
            return;
        }
    }
    TEST_ASSERT_LESS_THAN(LEVELS_MAX, s_fake.n_levels);
    strlcpy(s_fake.levels[s_fake.n_levels].tag, tag, LOGGING_TAG_MAX);
    s_fake.levels[s_fake.n_levels++].level = level;
}

/** Level ESP-IDF would filter `tag` at: its own level, else the "*" level; -1 when neither was set. */
static int level_of(const char *tag)
{
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < s_fake.n_levels; i++) {
            if (strcmp(s_fake.levels[i].tag, pass == 0 ? tag : "*") == 0) {
                return s_fake.levels[i].level;
            }
        }
    }
    return -1;
}

/** Whether `tag` has a level of its own (not just the "*" fallback). */
static bool has_own_level(const char *tag)
{
    for (int i = 0; i < s_fake.n_levels; i++) {
        if (strcmp(s_fake.levels[i].tag, tag) == 0) {
            return true;
        }
    }
    return false;
}

static void forget_level_calls(void)
{
    s_fake.n_levels = 0;
    s_fake.level_calls = 0;
}

static const logging_ops_t FAKE_OPS = {
    .blob_get = fake_blob_get,
    .blob_set = fake_blob_set,
    .level_set = fake_level_set,
};

/** Fresh fakes, empty NVS, component uninitialised. */
static void setup(void)
{
    logging_reset();
    memset(&s_fake, 0, sizeof(s_fake));
    logging_set_ops(&FAKE_OPS);
}

static void store_blob(uint8_t version, const logging_settings_t *s, size_t len)
{
    strcpy(s_fake.ns, LOGGING_NVS_NAMESPACE);
    strcpy(s_fake.key, LOGGING_NVS_KEY);
    memset(s_fake.blob, 0, sizeof(s_fake.blob));
    s_fake.blob[0] = version;
    memcpy(s_fake.blob + 1, s, sizeof(*s));
    s_fake.blob_len = len;
}

static logging_settings_t defaults(void)
{
    logging_settings_t s;

    logging_core_defaults(&s);
    return s;
}

/* ---- Warning capture (keeps the test output clean) ---- */

static int s_warnings;

static int count_warnings(const char *fmt, va_list ap)
{
    char text[256];
    int n = vsnprintf(text, sizeof(text), fmt, ap);

    // The logger may print a line in pieces; the prefix piece is "W (<time>) logging: ", possibly coloured.
    if (strstr(text, "W (") != NULL && strstr(text, "logging") != NULL) {
        s_warnings++;
    }
    return n;
}

static void init_counting_warnings(void)
{
    s_warnings = 0;
    vprintf_like_t prev = esp_log_set_vprintf(count_warnings);

    TEST_ASSERT_EQUAL(ESP_OK, logging_init());
    esp_log_set_vprintf(prev);
}

/* ---- Listeners ---- */

static struct {
    int calls;
    esp_log_level_t level;
    TaskHandle_t task;
    esp_err_t get_result; /**< logging_get_settings from inside the callback: proves the lock is released. */
} s_zig, s_ot;

static void on_zig(esp_log_level_t effective)
{
    logging_settings_t tmp;

    s_zig.calls++;
    s_zig.level = effective;
    s_zig.task = xTaskGetCurrentTaskHandle();
    s_zig.get_result = logging_get_settings(&tmp);
}

static void on_ot(esp_log_level_t effective)
{
    s_ot.calls++;
    s_ot.level = effective;
}

/* ---- logging_apply ---- */

TEST_CASE("apply valid settings: stored in NVS namespace logging as one blob with a version byte", "[logging]")
{
    setup();
    TEST_ASSERT_EQUAL(ESP_OK, logging_init());
    logging_settings_t s = defaults();

    s.channel[LOGGING_CH_SYSLOG] = ESP_LOG_WARN;
    strcpy(s.syslog_target, "192.168.1.10");
    logging_core_set_component(&s, "ziggurat", ESP_LOG_DEBUG);
    TEST_ASSERT_EQUAL(ESP_OK, logging_apply(&s, NULL));

    TEST_ASSERT_EQUAL(1, s_fake.sets);
    TEST_ASSERT_EQUAL_STRING("logging", s_fake.ns);
    TEST_ASSERT_EQUAL_STRING("settings", s_fake.key);
    TEST_ASSERT_EQUAL(1 + sizeof(logging_settings_t), s_fake.blob_len);
    TEST_ASSERT_EQUAL_UINT8(1, s_fake.blob[0]);
    TEST_ASSERT_EQUAL_MEMORY(&s, s_fake.blob + 1, sizeof(s));
}

TEST_CASE("apply valid settings: esp_log_level_set(\"*\", gate) and for each override tag", "[logging]")
{
    setup();
    TEST_ASSERT_EQUAL(ESP_OK, logging_init());
    forget_level_calls();
    logging_settings_t s = defaults();

    s.channel[LOGGING_CH_UART] = ESP_LOG_DEBUG;
    logging_core_set_component(&s, "ziggurat", ESP_LOG_VERBOSE);
    logging_core_set_component(&s, "wifi", ESP_LOG_ERROR);
    TEST_ASSERT_EQUAL(ESP_OK, logging_apply(&s, NULL));

    TEST_ASSERT_EQUAL(ESP_LOG_INFO, level_of("*"));
    TEST_ASSERT_EQUAL(ESP_LOG_DEBUG, level_of("ziggurat")); // capped by the highest channel
    TEST_ASSERT_EQUAL(ESP_LOG_ERROR, level_of("wifi"));
    TEST_ASSERT_EQUAL(3, s_fake.level_calls);
}

TEST_CASE("apply valid settings: each listener of a changed tag is called with its new effective level", "[logging]")
{
    setup();
    TEST_ASSERT_EQUAL(ESP_OK, logging_init());
    memset(&s_zig, 0, sizeof(s_zig));
    memset(&s_ot, 0, sizeof(s_ot));
    TEST_ASSERT_EQUAL(ESP_OK, logging_level_listener_add("ziggurat", on_zig));
    TEST_ASSERT_EQUAL(ESP_OK, logging_level_listener_add("OPENTHREAD", on_ot));
    s_zig.calls = 0; // the call on add is covered by its own test
    s_ot.calls = 0;
    logging_settings_t s = defaults();

    s.channel[LOGGING_CH_JTAG] = ESP_LOG_DEBUG;
    logging_core_set_component(&s, "ziggurat", ESP_LOG_DEBUG);
    TEST_ASSERT_EQUAL(ESP_OK, logging_apply(&s, NULL));

    TEST_ASSERT_EQUAL(1, s_zig.calls);
    TEST_ASSERT_EQUAL(ESP_LOG_DEBUG, s_zig.level);
    TEST_ASSERT_EQUAL_PTR(xTaskGetCurrentTaskHandle(), s_zig.task);
    TEST_ASSERT_EQUAL(ESP_OK, s_zig.get_result);
    TEST_ASSERT_EQUAL(0, s_ot.calls); // OPENTHREAD stays at info

    TEST_ASSERT_EQUAL(ESP_OK, logging_apply(&s, NULL)); // same settings again: nothing changed
    TEST_ASSERT_EQUAL(1, s_zig.calls);
}

TEST_CASE("apply and viewers change: overrides keep their own level after ESP-IDF's \"*\" call", "[logging]")
{
    setup();
    TEST_ASSERT_EQUAL(ESP_OK, logging_init());
    logging_settings_t s = defaults();

    s.default_level = ESP_LOG_WARN;
    logging_core_set_component(&s, "ziggurat", ESP_LOG_ERROR);
    logging_core_set_component(&s, "wifi", ESP_LOG_NONE);
    TEST_ASSERT_EQUAL(ESP_OK, logging_apply(&s, NULL));
    TEST_ASSERT_TRUE(has_own_level("ziggurat"));
    TEST_ASSERT_EQUAL(ESP_LOG_ERROR, level_of("ziggurat"));
    TEST_ASSERT_EQUAL(ESP_LOG_NONE, level_of("wifi"));
    TEST_ASSERT_EQUAL(ESP_LOG_WARN, level_of("*"));

    logging_viewers_max_changed(ESP_LOG_DEBUG);
    TEST_ASSERT_TRUE(has_own_level("ziggurat"));
    TEST_ASSERT_EQUAL(ESP_LOG_ERROR, level_of("ziggurat"));
    TEST_ASSERT_EQUAL(ESP_LOG_NONE, level_of("wifi"));
    TEST_ASSERT_EQUAL(ESP_LOG_WARN, level_of("*"));
}

/* ---- logging_level_listener_add ---- */

TEST_CASE("listener added: called once at once with the tag's current effective level", "[logging]")
{
    setup();
    TEST_ASSERT_EQUAL(ESP_OK, logging_init());
    logging_settings_t s = defaults();

    s.channel[LOGGING_CH_UART] = ESP_LOG_DEBUG;
    logging_core_set_component(&s, "ziggurat", ESP_LOG_VERBOSE);
    TEST_ASSERT_EQUAL(ESP_OK, logging_apply(&s, NULL));
    memset(&s_zig, 0, sizeof(s_zig));
    TEST_ASSERT_EQUAL(ESP_OK, logging_level_listener_add("ziggurat", on_zig));
    TEST_ASSERT_EQUAL(1, s_zig.calls);
    TEST_ASSERT_EQUAL(ESP_LOG_DEBUG, s_zig.level); // verbose capped by the highest channel
    TEST_ASSERT_EQUAL_PTR(xTaskGetCurrentTaskHandle(), s_zig.task);
    TEST_ASSERT_EQUAL(ESP_OK, s_zig.get_result); // called outside the lock
}

TEST_CASE("listener added: not called again by an apply that leaves its level unchanged", "[logging]")
{
    setup();
    TEST_ASSERT_EQUAL(ESP_OK, logging_init());
    memset(&s_zig, 0, sizeof(s_zig));
    TEST_ASSERT_EQUAL(ESP_OK, logging_level_listener_add("ziggurat", on_zig));
    TEST_ASSERT_EQUAL(1, s_zig.calls);
    logging_settings_t s = defaults();

    s.channel[LOGGING_CH_SYSLOG] = ESP_LOG_WARN; // changes the settings, not ziggurat's info
    strcpy(s.syslog_target, "192.168.1.10");
    logging_core_set_component(&s, "wifi", ESP_LOG_ERROR);
    TEST_ASSERT_EQUAL(ESP_OK, logging_apply(&s, NULL));
    TEST_ASSERT_EQUAL(1, s_zig.calls);
}

TEST_CASE("apply valid settings: returns ESP_OK", "[logging]")
{
    setup();
    TEST_ASSERT_EQUAL(ESP_OK, logging_init());
    logging_settings_t s = defaults();
    logging_settings_t got;
    const char *field = "untouched";

    s.channel[LOGGING_CH_UART] = ESP_LOG_WARN;
    TEST_ASSERT_EQUAL(ESP_OK, logging_apply(&s, &field));
    TEST_ASSERT_EQUAL(ESP_OK, logging_get_settings(&got));
    TEST_ASSERT_EQUAL_MEMORY(&s, &got, sizeof(s));
}

TEST_CASE("apply invalid settings: returns ESP_ERR_INVALID_ARG with the field, nothing stored or applied", "[logging]")
{
    setup();
    TEST_ASSERT_EQUAL(ESP_OK, logging_init());
    forget_level_calls();
    logging_settings_t s = defaults();
    logging_settings_t got;
    const char *field = NULL;

    s.channel[LOGGING_CH_UART] = (esp_log_level_t) 9;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, logging_apply(&s, &field));
    TEST_ASSERT_EQUAL_STRING("channels.uart.level", field);
    TEST_ASSERT_EQUAL(0, s_fake.sets);
    TEST_ASSERT_EQUAL(0, s_fake.level_calls);
    TEST_ASSERT_EQUAL(ESP_OK, logging_get_settings(&got));
    s = defaults();
    TEST_ASSERT_EQUAL_MEMORY(&s, &got, sizeof(s));
}

TEST_CASE("apply above the build's ceiling (info): ESP_ERR_INVALID_ARG with the field, nothing stored or applied",
          "[logging]")
{
    setup();
    logging_set_ceiling(ESP_LOG_INFO);
    TEST_ASSERT_EQUAL(ESP_OK, logging_init());
    forget_level_calls();
    logging_settings_t s = defaults();
    logging_settings_t got;
    logging_settings_t want = defaults();
    const char *field = NULL;

    logging_core_set_component(&s, "ziggurat", ESP_LOG_DEBUG);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, logging_apply(&s, &field));
    TEST_ASSERT_EQUAL_STRING("components.ziggurat", field);
    TEST_ASSERT_EQUAL(0, s_fake.sets);
    TEST_ASSERT_EQUAL(0, s_fake.level_calls);
    TEST_ASSERT_EQUAL(ESP_OK, logging_get_settings(&got));
    TEST_ASSERT_EQUAL_MEMORY(&want, &got, sizeof(got));
}

TEST_CASE("apply at the build's ceiling (info): accepted", "[logging]")
{
    setup();
    logging_set_ceiling(ESP_LOG_INFO);
    TEST_ASSERT_EQUAL(ESP_OK, logging_init());
    logging_settings_t s = defaults();

    logging_core_set_component(&s, "ziggurat", ESP_LOG_INFO);
    TEST_ASSERT_EQUAL(ESP_OK, logging_apply(&s, NULL));
    TEST_ASSERT_EQUAL(ESP_LOG_INFO, logging_level_ceiling());
}

TEST_CASE("apply on NVS write failure: returns ESP_FAIL, levels still applied", "[logging]")
{
    setup();
    TEST_ASSERT_EQUAL(ESP_OK, logging_init());
    forget_level_calls();
    s_fake.set_result = ESP_ERR_NO_MEM;
    logging_settings_t s = defaults();
    logging_settings_t got;

    s.default_level = ESP_LOG_WARN;
    TEST_ASSERT_EQUAL(ESP_FAIL, logging_apply(&s, NULL));
    TEST_ASSERT_EQUAL(1, s_fake.sets);
    TEST_ASSERT_EQUAL(ESP_LOG_WARN, level_of("*"));
    TEST_ASSERT_EQUAL(ESP_OK, logging_get_settings(&got));
    TEST_ASSERT_EQUAL(ESP_LOG_WARN, got.default_level);
}

TEST_CASE("apply with a removed override: esp_log_level_set(tag, gate of the default)", "[logging]")
{
    setup();
    TEST_ASSERT_EQUAL(ESP_OK, logging_init());
    logging_settings_t s = defaults();

    s.channel[LOGGING_CH_UART] = ESP_LOG_DEBUG;
    logging_core_set_component(&s, "ziggurat", ESP_LOG_DEBUG);
    TEST_ASSERT_EQUAL(ESP_OK, logging_apply(&s, NULL));
    TEST_ASSERT_EQUAL(ESP_LOG_DEBUG, level_of("ziggurat"));

    forget_level_calls();
    logging_core_remove_component(&s, "ziggurat");
    s.default_level = ESP_LOG_WARN;
    TEST_ASSERT_EQUAL(ESP_OK, logging_apply(&s, NULL));
    TEST_ASSERT_EQUAL(ESP_LOG_WARN, level_of("ziggurat"));
    TEST_ASSERT_EQUAL(ESP_LOG_WARN, level_of("*"));
}

#define APPLY_ROUNDS 1000

static logging_settings_t s_input[2];
static volatile int s_done;

static void apply_task(void *arg)
{
    const logging_settings_t *s = arg;

    for (int i = 0; i < APPLY_ROUNDS; i++) {
        TEST_ASSERT_EQUAL(ESP_OK, logging_apply(s, NULL));
        if (i % 16 == 0) {
            taskYIELD();
        }
    }
    s_done++;
    vTaskDelete(NULL);
}

TEST_CASE("concurrent apply from two tasks: last writer wins whole, stored blob equals one input", "[logging]")
{
    setup();
    TEST_ASSERT_EQUAL(ESP_OK, logging_init());
    s_input[0] = defaults();
    s_input[0].channel[LOGGING_CH_UART] = ESP_LOG_DEBUG;
    s_input[0].default_level = ESP_LOG_DEBUG;
    logging_core_set_component(&s_input[0], "ziggurat", ESP_LOG_ERROR);
    s_input[1] = defaults();
    s_input[1].channel[LOGGING_CH_SYSLOG] = ESP_LOG_WARN;
    strcpy(s_input[1].syslog_target, "logs.lan:1514");
    s_input[1].default_level = ESP_LOG_WARN;
    logging_core_set_component(&s_input[1], "wifi", ESP_LOG_NONE);
    logging_core_set_component(&s_input[1], "OPENTHREAD", ESP_LOG_VERBOSE);

    s_done = 0;
    s_fake.inputs = s_input;
    UBaseType_t prio = uxTaskPriorityGet(NULL);

    TEST_ASSERT_EQUAL(pdPASS, xTaskCreate(apply_task, "apply_a", 4096, &s_input[0], prio, NULL));
    TEST_ASSERT_EQUAL(pdPASS, xTaskCreate(apply_task, "apply_b", 4096, &s_input[1], prio, NULL));
    for (int i = 0; i < 1000 && s_done < 2; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    TEST_ASSERT_EQUAL(2, s_done);
    TEST_ASSERT_EQUAL(2 * APPLY_ROUNDS, s_fake.sets);
    TEST_ASSERT_EQUAL(0, s_fake.overlaps);
    TEST_ASSERT_EQUAL(0, s_fake.mixed);

    logging_settings_t got;

    TEST_ASSERT_EQUAL(ESP_OK, logging_get_settings(&got));
    int winner = memcmp(&got, &s_input[0], sizeof(got)) == 0 ? 0 : 1;

    TEST_ASSERT_EQUAL_MEMORY(&s_input[winner], &got, sizeof(got));
    TEST_ASSERT_EQUAL(LOGGING_BLOB_SIZE, s_fake.blob_len);
    TEST_ASSERT_EQUAL_MEMORY(&s_input[winner], s_fake.blob + 1, sizeof(got));
    TEST_ASSERT_EQUAL(s_input[winner].default_level, level_of("*"));
}

/* ---- logging_init ---- */

TEST_CASE("init on an empty NVS: defaults applied", "[logging]")
{
    setup();
    init_counting_warnings();
    logging_settings_t got;
    logging_settings_t want = defaults();

    TEST_ASSERT_EQUAL(ESP_OK, logging_get_settings(&got));
    TEST_ASSERT_EQUAL_MEMORY(&want, &got, sizeof(got));
    TEST_ASSERT_EQUAL(ESP_LOG_INFO, level_of("*"));
    TEST_ASSERT_EQUAL(0, s_fake.sets);
    TEST_ASSERT_EQUAL(0, s_warnings);
}

TEST_CASE("init on a stored valid blob: those settings applied", "[logging]")
{
    setup();
    logging_settings_t s = defaults();
    logging_settings_t got;

    s.channel[LOGGING_CH_UART] = ESP_LOG_DEBUG;
    logging_core_set_component(&s, "ziggurat", ESP_LOG_DEBUG);
    store_blob(LOGGING_BLOB_VERSION, &s, LOGGING_BLOB_SIZE);
    init_counting_warnings();
    TEST_ASSERT_EQUAL(ESP_OK, logging_get_settings(&got));
    TEST_ASSERT_EQUAL_MEMORY(&s, &got, sizeof(got));
    TEST_ASSERT_EQUAL(ESP_LOG_DEBUG, level_of("ziggurat"));
    TEST_ASSERT_EQUAL(0, s_warnings);
}

TEST_CASE("init on a stored blob of an unknown version: defaults applied, warning logged once", "[logging]")
{
    setup();
    logging_settings_t s = defaults();
    logging_settings_t got;
    logging_settings_t want = defaults();

    s.default_level = ESP_LOG_ERROR;
    store_blob(2, &s, LOGGING_BLOB_SIZE);
    init_counting_warnings();
    TEST_ASSERT_EQUAL(ESP_OK, logging_get_settings(&got));
    TEST_ASSERT_EQUAL_MEMORY(&want, &got, sizeof(got));
    TEST_ASSERT_EQUAL(ESP_LOG_INFO, level_of("*"));
    TEST_ASSERT_EQUAL(1, s_warnings);
}

TEST_CASE("init on a stored blob of the wrong size: defaults applied, warning logged once", "[logging]")
{
    static const size_t sizes[] = {LOGGING_BLOB_SIZE - 4, LOGGING_BLOB_SIZE + 4};

    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        setup();
        logging_settings_t s = defaults();
        logging_settings_t got;
        logging_settings_t want = defaults();

        s.default_level = ESP_LOG_ERROR;
        store_blob(LOGGING_BLOB_VERSION, &s, sizes[i]);
        init_counting_warnings();
        TEST_ASSERT_EQUAL(ESP_OK, logging_get_settings(&got));
        TEST_ASSERT_EQUAL_MEMORY(&want, &got, sizeof(got));
        TEST_ASSERT_EQUAL(1, s_warnings);
    }
}

TEST_CASE("init on a stored blob that fails validation: defaults applied, warning logged once", "[logging]")
{
    setup();
    logging_settings_t s = defaults();
    logging_settings_t got;
    logging_settings_t want = defaults();

    strcpy(s.components[0].tag, "*");
    s.components[0].level = ESP_LOG_DEBUG;
    s.n_components = 1;
    store_blob(LOGGING_BLOB_VERSION, &s, LOGGING_BLOB_SIZE);
    init_counting_warnings();
    TEST_ASSERT_EQUAL(ESP_OK, logging_get_settings(&got));
    TEST_ASSERT_EQUAL_MEMORY(&want, &got, sizeof(got));
    TEST_ASSERT_EQUAL(1, s_warnings);
}

TEST_CASE("init on a stored blob with levels above the build's ceiling (info): lowered to info, warning logged once",
          "[logging]")
{
    setup();
    logging_set_ceiling(ESP_LOG_INFO);
    logging_settings_t s = defaults();
    logging_settings_t got;
    logging_settings_t want;

    s.channel[LOGGING_CH_UART] = ESP_LOG_VERBOSE;
    logging_core_set_component(&s, "ziggurat", ESP_LOG_DEBUG);
    logging_core_set_component(&s, "OPENTHREAD", ESP_LOG_WARN);
    want = s;
    want.channel[LOGGING_CH_UART] = ESP_LOG_INFO;
    want.components[0].level = ESP_LOG_INFO;
    store_blob(LOGGING_BLOB_VERSION, &s, LOGGING_BLOB_SIZE);
    init_counting_warnings();
    TEST_ASSERT_EQUAL(ESP_OK, logging_get_settings(&got));
    TEST_ASSERT_EQUAL_MEMORY(&want, &got, sizeof(got));
    TEST_ASSERT_EQUAL(ESP_LOG_INFO, level_of("ziggurat"));
    TEST_ASSERT_EQUAL(ESP_LOG_WARN, level_of("OPENTHREAD"));
    TEST_ASSERT_EQUAL(1, s_warnings);
    TEST_ASSERT_EQUAL(0, s_fake.sets);
}

/* ---- logging_viewers_max_changed ---- */

TEST_CASE("first debug viewer: gates recomputed with viewers_max debug", "[logging]")
{
    setup();
    TEST_ASSERT_EQUAL(ESP_OK, logging_init());
    logging_settings_t s = defaults();

    logging_core_set_component(&s, "ziggurat", ESP_LOG_DEBUG);
    TEST_ASSERT_EQUAL(ESP_OK, logging_apply(&s, NULL));
    memset(&s_zig, 0, sizeof(s_zig));
    TEST_ASSERT_EQUAL(ESP_OK, logging_level_listener_add("ziggurat", on_zig));
    TEST_ASSERT_EQUAL(ESP_LOG_INFO, level_of("ziggurat"));

    logging_viewers_max_changed(ESP_LOG_DEBUG);
    TEST_ASSERT_EQUAL(ESP_LOG_DEBUG, level_of("ziggurat"));
    TEST_ASSERT_EQUAL(ESP_LOG_INFO, level_of("*"));
    TEST_ASSERT_EQUAL(2, s_zig.calls); // once on add, once for the change
    TEST_ASSERT_EQUAL(ESP_LOG_DEBUG, s_zig.level);
}

TEST_CASE("last viewer closing: gates recomputed with viewers_max off", "[logging]")
{
    setup();
    TEST_ASSERT_EQUAL(ESP_OK, logging_init());
    logging_settings_t s = defaults();

    s.default_level = ESP_LOG_DEBUG;
    logging_core_set_component(&s, "ziggurat", ESP_LOG_VERBOSE);
    TEST_ASSERT_EQUAL(ESP_OK, logging_apply(&s, NULL));
    logging_viewers_max_changed(ESP_LOG_VERBOSE);
    TEST_ASSERT_EQUAL(ESP_LOG_DEBUG, level_of("*"));
    TEST_ASSERT_EQUAL(ESP_LOG_VERBOSE, level_of("ziggurat"));

    logging_viewers_max_changed(ESP_LOG_NONE);
    TEST_ASSERT_EQUAL(ESP_LOG_INFO, level_of("*"));
    TEST_ASSERT_EQUAL(ESP_LOG_INFO, level_of("ziggurat"));
}

/* ---- Levels published for the capture (read without the settings lock) ---- */

TEST_CASE("before logging_init: published levels are all off", "[logging]")
{
    setup();
    TEST_ASSERT_EQUAL_UINT32(0, logging_levels().word);
}

TEST_CASE("init on an empty NVS: default channel levels published", "[logging]")
{
    setup();
    TEST_ASSERT_EQUAL(ESP_OK, logging_init());
    logging_levels_t lv = logging_levels();

    TEST_ASSERT_EQUAL(ESP_LOG_INFO, lv.channel[LOGGING_CH_UART]);
    TEST_ASSERT_EQUAL(ESP_LOG_INFO, lv.channel[LOGGING_CH_JTAG]);
    TEST_ASSERT_EQUAL(ESP_LOG_NONE, lv.channel[LOGGING_CH_SYSLOG]);
    TEST_ASSERT_EQUAL(ESP_LOG_NONE, lv.viewers);
}

TEST_CASE("apply valid settings: channel levels published, generation + 1", "[logging]")
{
    setup();
    TEST_ASSERT_EQUAL(ESP_OK, logging_init());
    uint32_t gen = logging_settings_generation();
    logging_settings_t s = defaults();

    s.channel[LOGGING_CH_JTAG] = ESP_LOG_DEBUG;
    s.channel[LOGGING_CH_SYSLOG] = ESP_LOG_WARN;
    strcpy(s.syslog_target, "192.168.1.10");
    TEST_ASSERT_EQUAL(ESP_OK, logging_apply(&s, NULL));

    TEST_ASSERT_EQUAL(ESP_LOG_DEBUG, logging_levels().channel[LOGGING_CH_JTAG]);
    TEST_ASSERT_EQUAL(ESP_LOG_WARN, logging_levels().channel[LOGGING_CH_SYSLOG]);
    TEST_ASSERT_EQUAL_UINT32(gen + 1, logging_settings_generation());
}

TEST_CASE("apply invalid settings: published levels and generation unchanged", "[logging]")
{
    setup();
    TEST_ASSERT_EQUAL(ESP_OK, logging_init());
    uint32_t gen = logging_settings_generation();
    uint32_t word = logging_levels().word;
    logging_settings_t s = defaults();

    s.channel[LOGGING_CH_JTAG] = (esp_log_level_t) 9;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, logging_apply(&s, NULL));
    TEST_ASSERT_EQUAL_UINT32(word, logging_levels().word);
    TEST_ASSERT_EQUAL_UINT32(gen, logging_settings_generation());
}

TEST_CASE("first debug viewer: viewers level published", "[logging]")
{
    setup();
    TEST_ASSERT_EQUAL(ESP_OK, logging_init());
    logging_viewers_max_changed(ESP_LOG_DEBUG);
    TEST_ASSERT_EQUAL(ESP_LOG_DEBUG, logging_levels().viewers);
    logging_viewers_max_changed(ESP_LOG_NONE);
    TEST_ASSERT_EQUAL(ESP_LOG_NONE, logging_levels().viewers);
}
