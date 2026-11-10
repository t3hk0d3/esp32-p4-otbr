#include <stdio.h>
#include <string.h>
#include "unity.h"
#include "logging_core.h"

static logging_settings_t fresh(void)
{
    logging_settings_t s;

    logging_core_defaults(&s);
    return s;
}

/* Runs the parser on a fixed argv; `err` is set to NULL first so a test can tell whether it was written. */
static esp_err_t run(logging_settings_t *s, const char **err, int argc, const char *a1, const char *a2, const char *a3)
{
    char *argv[4] = {(char *) "log", (char *) a1, (char *) a2, (char *) a3};

    *err = NULL;
    return logging_console_parse(argc, argv, s, err);
}

/* ---- log level / log tag / log syslog ---- */

TEST_CASE("log level syslog warning: the syslog channel level is set", "[logging]")
{
    logging_settings_t s = fresh();
    const char *err;

    TEST_ASSERT_EQUAL(ESP_OK, run(&s, &err, 4, "level", "syslog", "warning"));
    TEST_ASSERT_EQUAL(ESP_LOG_WARN, s.channel[LOGGING_CH_SYSLOG]);
    TEST_ASSERT_EQUAL(ESP_LOG_INFO, s.channel[LOGGING_CH_UART]);
    TEST_ASSERT_EQUAL(ESP_LOG_INFO, s.channel[LOGGING_CH_JTAG]);
}

TEST_CASE("log level uart / jtag: each channel is addressed by its name", "[logging]")
{
    logging_settings_t s = fresh();
    const char *err;

    TEST_ASSERT_EQUAL(ESP_OK, run(&s, &err, 4, "level", "uart", "error"));
    TEST_ASSERT_EQUAL(ESP_OK, run(&s, &err, 4, "level", "jtag", "debug"));
    TEST_ASSERT_EQUAL(ESP_LOG_ERROR, s.channel[LOGGING_CH_UART]);
    TEST_ASSERT_EQUAL(ESP_LOG_DEBUG, s.channel[LOGGING_CH_JTAG]);
}

TEST_CASE("log tag ziggurat debug: an override is added", "[logging]")
{
    logging_settings_t s = fresh();
    const char *err;

    TEST_ASSERT_EQUAL(ESP_OK, run(&s, &err, 4, "tag", "ziggurat", "debug"));
    TEST_ASSERT_EQUAL_UINT8(1, s.n_components);
    TEST_ASSERT_EQUAL_STRING("ziggurat", s.components[0].tag);
    TEST_ASSERT_EQUAL(ESP_LOG_DEBUG, s.components[0].level);
}

TEST_CASE("log tag on a tag with an override: the level is replaced, the count unchanged", "[logging]")
{
    logging_settings_t s = fresh();
    const char *err;

    TEST_ASSERT_EQUAL(ESP_OK, run(&s, &err, 4, "tag", "ziggurat", "debug"));
    TEST_ASSERT_EQUAL(ESP_OK, run(&s, &err, 4, "tag", "ziggurat", "error"));
    TEST_ASSERT_EQUAL_UINT8(1, s.n_components);
    TEST_ASSERT_EQUAL(ESP_LOG_ERROR, s.components[0].level);
}

TEST_CASE("log tag ziggurat default: the override is removed", "[logging]")
{
    logging_settings_t s = fresh();
    const char *err;

    TEST_ASSERT_EQUAL(ESP_OK, run(&s, &err, 4, "tag", "ziggurat", "debug"));
    TEST_ASSERT_EQUAL(ESP_OK, run(&s, &err, 4, "tag", "ziggurat", "default"));
    TEST_ASSERT_EQUAL_UINT8(0, s.n_components);
}

TEST_CASE("log tag * warning: the default level is set", "[logging]")
{
    logging_settings_t s = fresh();
    const char *err;

    TEST_ASSERT_EQUAL(ESP_OK, run(&s, &err, 4, "tag", "*", "warning"));
    TEST_ASSERT_EQUAL(ESP_LOG_WARN, s.default_level);
    TEST_ASSERT_EQUAL_UINT8(0, s.n_components);
}

TEST_CASE("log syslog 10.0.0.2: the target is set, the level stays off", "[logging]")
{
    logging_settings_t s = fresh();
    const char *err;

    TEST_ASSERT_EQUAL(ESP_OK, run(&s, &err, 3, "syslog", "10.0.0.2", NULL));
    TEST_ASSERT_EQUAL_STRING("10.0.0.2", s.syslog_target);
    TEST_ASSERT_EQUAL(ESP_LOG_NONE, s.channel[LOGGING_CH_SYSLOG]);
}

TEST_CASE("log syslog 10.0.0.2 with syslog on: the level stays as it was", "[logging]")
{
    logging_settings_t s = fresh();
    const char *err;

    s.channel[LOGGING_CH_SYSLOG] = ESP_LOG_WARN;
    TEST_ASSERT_EQUAL(ESP_OK, run(&s, &err, 3, "syslog", "[fd00::1]:1514", NULL));
    TEST_ASSERT_EQUAL_STRING("[fd00::1]:1514", s.syslog_target);
    TEST_ASSERT_EQUAL(ESP_LOG_WARN, s.channel[LOGGING_CH_SYSLOG]);
}

TEST_CASE("log syslog off: the target is cleared and the syslog level is off", "[logging]")
{
    logging_settings_t s = fresh();
    const char *err;

    s.channel[LOGGING_CH_SYSLOG] = ESP_LOG_WARN;
    strcpy(s.syslog_target, "10.0.0.2");
    TEST_ASSERT_EQUAL(ESP_OK, run(&s, &err, 3, "syslog", "off", NULL));
    TEST_ASSERT_EQUAL_STRING("", s.syslog_target);
    TEST_ASSERT_EQUAL(ESP_LOG_NONE, s.channel[LOGGING_CH_SYSLOG]);
}

/* ---- bad input ---- */

TEST_CASE("bad input: ESP_ERR_INVALID_ARG with a message, settings untouched", "[logging]")
{
    static const struct {
        int argc;
        const char *a1, *a2, *a3;
    } bad[] = {
        {2, "level", NULL, NULL},               /* missing channel and level */
        {3, "level", "uart", NULL},             /* missing level */
        {4, "level", "usb", "info"},            /* unknown channel */
        {4, "level", "uart", "warn"},           /* unknown level */
        {5, "level", "uart", "info"},           /* argc counts one extra argument below */
        {3, "tag", "ziggurat", NULL},           /* missing level */
        {4, "tag", "ziggurat", "loud"},         /* unknown level */
        {4, "tag", "", "info"},                 /* empty tag */
        {4, "tag", "0123456789012345678901234", "info"}, /* tag of 25 bytes */
        {4, "tag", "*", "default"},             /* the default has no default */
        {4, "tag", "never_set", "default"},     /* no override to remove */
        {2, "syslog", NULL, NULL},              /* missing target */
        {3, "syslog", "host:0", NULL},          /* port out of range */
        {3, "syslog", "fd00::1:514", NULL},     /* unbracketed IPv6 */
        {3, "frobnicate", "x", NULL},           /* unknown subcommand */
    };

    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        logging_settings_t s = fresh();
        logging_settings_t before = s;
        const char *err;
        char *argv[6] = {(char *) "log", (char *) bad[i].a1, (char *) bad[i].a2, (char *) bad[i].a3, (char *) "extra",
                         NULL
                        };
        int argc = bad[i].argc;

        err = NULL;
        TEST_ASSERT_EQUAL_MESSAGE(ESP_ERR_INVALID_ARG, logging_console_parse(argc, argv, &s, &err), bad[i].a1);
        TEST_ASSERT_NOT_NULL(err);
        TEST_ASSERT_TRUE(err[0] != '\0');
        TEST_ASSERT_EQUAL_MEMORY(&before, &s, sizeof(s));
    }
}

TEST_CASE("bad input after a good change in the same struct: nothing changes", "[logging]")
{
    logging_settings_t s = fresh();
    logging_settings_t before;
    const char *err;

    TEST_ASSERT_EQUAL(ESP_OK, run(&s, &err, 4, "tag", "ziggurat", "debug"));
    before = s;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, run(&s, &err, 4, "tag", "ziggurat", "loud"));
    TEST_ASSERT_EQUAL_MEMORY(&before, &s, sizeof(s));
}

TEST_CASE("log tag with 33 overrides: ESP_ERR_NO_MEM, settings untouched", "[logging]")
{
    logging_settings_t s = fresh();
    logging_settings_t before;
    const char *err;
    char tag[LOGGING_TAG_MAX];

    for (int i = 0; i < LOGGING_COMPONENTS_MAX; i++) {
        snprintf(tag, sizeof(tag), "t%d", i);
        TEST_ASSERT_EQUAL(ESP_OK, run(&s, &err, 4, "tag", tag, "debug"));
    }
    before = s;
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, run(&s, &err, 4, "tag", "one_too_many", "debug"));
    TEST_ASSERT_NOT_NULL(err);
    TEST_ASSERT_EQUAL_MEMORY(&before, &s, sizeof(s));
}
