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

/* ---- logging_level_name / logging_level_parse ---- */

TEST_CASE("each of off, error, warning, info, debug, verbose: name and parse round-trip", "[logging]")
{
    static const char *const names[] = {"off", "error", "warning", "info", "debug", "verbose"};

    for (int i = 0; i < 6; i++) {
        esp_log_level_t lvl = (esp_log_level_t) 99;

        TEST_ASSERT_EQUAL_STRING(names[i], logging_level_name((esp_log_level_t) i));
        TEST_ASSERT_EQUAL(ESP_OK, logging_level_parse(names[i], &lvl));
        TEST_ASSERT_EQUAL(i, lvl);
    }
    TEST_ASSERT_EQUAL(ESP_LOG_NONE, 0);
    TEST_ASSERT_EQUAL(ESP_LOG_VERBOSE, 5);
}

TEST_CASE("unknown level text: parse returns ESP_ERR_INVALID_ARG, output untouched", "[logging]")
{
    static const char *const bad[] = {"warn", "", "INFO ", "Info", "6"};

    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        esp_log_level_t lvl = ESP_LOG_DEBUG;

        TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, logging_level_parse(bad[i], &lvl));
        TEST_ASSERT_EQUAL(ESP_LOG_DEBUG, lvl);
    }
    esp_log_level_t lvl = ESP_LOG_INFO;

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, logging_level_parse(NULL, &lvl));
}

/* ---- logging_core_defaults ---- */

TEST_CASE("fresh settings struct: UART info, JTAG info, syslog off, target empty, default info, no components", "[logging]")
{
    logging_settings_t s;

    memset(&s, 0xA5, sizeof(s));
    logging_core_defaults(&s);
    TEST_ASSERT_EQUAL(ESP_LOG_INFO, s.channel[LOGGING_CH_UART]);
    TEST_ASSERT_EQUAL(ESP_LOG_INFO, s.channel[LOGGING_CH_JTAG]);
    TEST_ASSERT_EQUAL(ESP_LOG_NONE, s.channel[LOGGING_CH_SYSLOG]);
    TEST_ASSERT_EQUAL_STRING("", s.syslog_target);
    TEST_ASSERT_EQUAL(ESP_LOG_INFO, s.default_level);
    TEST_ASSERT_EQUAL(0, s.n_components);
}

/* ---- logging_core_tag_level ---- */

TEST_CASE("tag with an override: tag_level returns the override", "[logging]")
{
    logging_settings_t s = fresh();

    TEST_ASSERT_EQUAL(ESP_OK, logging_core_set_component(&s, "ziggurat", ESP_LOG_DEBUG));
    TEST_ASSERT_EQUAL(ESP_LOG_DEBUG, logging_core_tag_level(&s, "ziggurat"));
}

TEST_CASE("tag without an override: tag_level returns default_level", "[logging]")
{
    logging_settings_t s = fresh();

    s.default_level = ESP_LOG_WARN;
    TEST_ASSERT_EQUAL(ESP_LOG_WARN, logging_core_tag_level(&s, "radio_mux"));
}

/* ---- logging_core_gate ---- */

TEST_CASE("ziggurat debug, syslog warning, UART info, no viewers: gate for ziggurat is info", "[logging]")
{
    logging_settings_t s = fresh();

    s.channel[LOGGING_CH_JTAG] = ESP_LOG_NONE;
    s.channel[LOGGING_CH_SYSLOG] = ESP_LOG_WARN;
    logging_core_set_component(&s, "ziggurat", ESP_LOG_DEBUG);
    TEST_ASSERT_EQUAL(ESP_LOG_INFO, logging_core_gate(&s, "ziggurat", ESP_LOG_NONE));
}

TEST_CASE("ziggurat debug and a debug viewer: gate for ziggurat is debug", "[logging]")
{
    logging_settings_t s = fresh();

    logging_core_set_component(&s, "ziggurat", ESP_LOG_DEBUG);
    TEST_ASSERT_EQUAL(ESP_LOG_DEBUG, logging_core_gate(&s, "ziggurat", ESP_LOG_DEBUG));
}

TEST_CASE("every channel off and no viewers: gate is off for every tag", "[logging]")
{
    logging_settings_t s = fresh();

    s.channel[LOGGING_CH_UART] = ESP_LOG_NONE;
    s.channel[LOGGING_CH_JTAG] = ESP_LOG_NONE;
    s.default_level = ESP_LOG_VERBOSE;
    logging_core_set_component(&s, "ziggurat", ESP_LOG_DEBUG);
    TEST_ASSERT_EQUAL(ESP_LOG_NONE, logging_core_gate(&s, "ziggurat", ESP_LOG_NONE));
    TEST_ASSERT_EQUAL(ESP_LOG_NONE, logging_core_gate(&s, "other", ESP_LOG_NONE));
    TEST_ASSERT_EQUAL(ESP_LOG_NONE, logging_core_channels_max(&s, ESP_LOG_NONE));
}

TEST_CASE("tag at error and a verbose viewer: gate is error (component caps it)", "[logging]")
{
    logging_settings_t s = fresh();

    logging_core_set_component(&s, "noisy", ESP_LOG_ERROR);
    TEST_ASSERT_EQUAL(ESP_LOG_ERROR, logging_core_gate(&s, "noisy", ESP_LOG_VERBOSE));
}

/* ---- logging_core_passes ---- */

TEST_CASE("line debug, component debug, channel warning: passes is false", "[logging]")
{
    TEST_ASSERT_FALSE(logging_core_passes(ESP_LOG_DEBUG, ESP_LOG_DEBUG, ESP_LOG_WARN));
}

TEST_CASE("line warning, component debug, channel warning: passes is true", "[logging]")
{
    TEST_ASSERT_TRUE(logging_core_passes(ESP_LOG_WARN, ESP_LOG_DEBUG, ESP_LOG_WARN));
}

/* ---- logging_core_set_component / logging_core_remove_component ---- */

TEST_CASE("new tag with room: appended, n_components + 1", "[logging]")
{
    logging_settings_t s = fresh();

    TEST_ASSERT_EQUAL(ESP_OK, logging_core_set_component(&s, "a", ESP_LOG_DEBUG));
    TEST_ASSERT_EQUAL(ESP_OK, logging_core_set_component(&s, "b", ESP_LOG_ERROR));
    TEST_ASSERT_EQUAL(2, s.n_components);
    TEST_ASSERT_EQUAL_STRING("b", s.components[1].tag);
    TEST_ASSERT_EQUAL(ESP_LOG_ERROR, s.components[1].level);
}

TEST_CASE("existing tag: level replaced, count unchanged", "[logging]")
{
    logging_settings_t s = fresh();

    logging_core_set_component(&s, "a", ESP_LOG_DEBUG);
    TEST_ASSERT_EQUAL(ESP_OK, logging_core_set_component(&s, "a", ESP_LOG_ERROR));
    TEST_ASSERT_EQUAL(1, s.n_components);
    TEST_ASSERT_EQUAL(ESP_LOG_ERROR, s.components[0].level);
}

TEST_CASE("33rd tag: ESP_ERR_NO_MEM, settings unchanged", "[logging]")
{
    logging_settings_t s = fresh();
    logging_settings_t before;
    char tag[8];

    for (int i = 0; i < LOGGING_COMPONENTS_MAX; i++) {
        snprintf(tag, sizeof(tag), "t%d", i);
        TEST_ASSERT_EQUAL(ESP_OK, logging_core_set_component(&s, tag, ESP_LOG_DEBUG));
    }
    before = s;
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, logging_core_set_component(&s, "one_more", ESP_LOG_DEBUG));
    TEST_ASSERT_EQUAL_MEMORY(&before, &s, sizeof(s));
    /* A full set still takes a replacement. */
    TEST_ASSERT_EQUAL(ESP_OK, logging_core_set_component(&s, "t0", ESP_LOG_ERROR));
}

TEST_CASE("tag of 24 or more bytes, empty, or \"*\": ESP_ERR_INVALID_ARG", "[logging]")
{
    logging_settings_t s = fresh();

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, logging_core_set_component(&s, "123456789012345678901234", ESP_LOG_INFO));
    TEST_ASSERT_EQUAL(ESP_OK, logging_core_set_component(&s, "12345678901234567890123", ESP_LOG_INFO));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, logging_core_set_component(&s, "", ESP_LOG_INFO));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, logging_core_set_component(&s, "*", ESP_LOG_INFO));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, logging_core_set_component(&s, NULL, ESP_LOG_INFO));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, logging_core_set_component(&s, "x", (esp_log_level_t) 6));
    TEST_ASSERT_EQUAL(1, s.n_components);
}

TEST_CASE("tag with a space, a quote, a backslash, a control or a non-ASCII byte: ESP_ERR_INVALID_ARG", "[logging]")
{
    const char *bad[] = {"has space", "say\"hi", "it's", "back\\slash", "tab\there", "nl\n", "del\x7f", "caf\xc3\xa9",
                         " lead", "trail "
                        };
    logging_settings_t s = fresh();

    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        TEST_ASSERT_EQUAL_MESSAGE(ESP_ERR_INVALID_ARG, logging_core_set_component(&s, bad[i], ESP_LOG_INFO), bad[i]);
    }
    TEST_ASSERT_EQUAL(0, s.n_components);
}

TEST_CASE("tag of printable ASCII (dots, dashes, brackets, tilde): accepted", "[logging]")
{
    const char *good[] = {"esp.emac", "esp-netif_lwip-ppp", "OPENTHREAD", "a[1]", "~!#$%&()*+,/:;<=>?@^`{|"};
    logging_settings_t s = fresh();

    for (size_t i = 0; i < sizeof(good) / sizeof(good[0]); i++) {
        TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, logging_core_set_component(&s, good[i], ESP_LOG_INFO), good[i]);
    }
}

TEST_CASE("settings with a quoted tag (NVS or PUT): ESP_ERR_INVALID_ARG, field components.<tag>", "[logging]")
{
    logging_settings_t s = fresh();
    const char *field = NULL;

    s.n_components = 1;
    strcpy(s.components[0].tag, "a\"b");
    s.components[0].level = ESP_LOG_INFO;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, logging_core_validate(&s, &field));
    TEST_ASSERT_EQUAL_STRING("components.a\"b", field);
}

TEST_CASE("removing a known tag: removed, others keep their order", "[logging]")
{
    logging_settings_t s = fresh();

    logging_core_set_component(&s, "a", ESP_LOG_DEBUG);
    logging_core_set_component(&s, "b", ESP_LOG_ERROR);
    logging_core_set_component(&s, "c", ESP_LOG_WARN);
    TEST_ASSERT_EQUAL(ESP_OK, logging_core_remove_component(&s, "b"));
    TEST_ASSERT_EQUAL(2, s.n_components);
    TEST_ASSERT_EQUAL_STRING("a", s.components[0].tag);
    TEST_ASSERT_EQUAL_STRING("c", s.components[1].tag);
    TEST_ASSERT_EQUAL(ESP_LOG_WARN, s.components[1].level);
}

TEST_CASE("removing an unknown tag: ESP_ERR_NOT_FOUND", "[logging]")
{
    logging_settings_t s = fresh();

    logging_core_set_component(&s, "a", ESP_LOG_DEBUG);
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, logging_core_remove_component(&s, "zz"));
    TEST_ASSERT_EQUAL(1, s.n_components);
}

/* ---- logging_core_validate ---- */

TEST_CASE("valid defaults and a valid syslog target: validate returns ESP_OK", "[logging]")
{
    logging_settings_t s = fresh();
    const char *field = NULL;

    TEST_ASSERT_EQUAL(ESP_OK, logging_core_validate(&s, &field));
    s.channel[LOGGING_CH_SYSLOG] = ESP_LOG_WARN;
    strcpy(s.syslog_target, "[fd00::1]:1514");
    TEST_ASSERT_EQUAL(ESP_OK, logging_core_validate(&s, NULL));
}

TEST_CASE("level outside 0..5 in a channel: ESP_ERR_INVALID_ARG, field channels.<name>.level", "[logging]")
{
    static const char *const fields[] = {"channels.uart.level", "channels.jtag.level", "channels.syslog.level"};

    for (int ch = 0; ch < LOGGING_CH_COUNT; ch++) {
        logging_settings_t s = fresh();
        const char *field = NULL;

        s.channel[ch] = (esp_log_level_t) 6;
        TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, logging_core_validate(&s, &field));
        TEST_ASSERT_EQUAL_STRING(fields[ch], field);
    }
}

TEST_CASE("level outside 0..5 in default: ESP_ERR_INVALID_ARG, field components.*", "[logging]")
{
    logging_settings_t s = fresh();
    const char *field = NULL;

    s.default_level = (esp_log_level_t) - 1;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, logging_core_validate(&s, &field));
    TEST_ASSERT_EQUAL_STRING("components.*", field);
}

TEST_CASE("level outside 0..5 in a component: ESP_ERR_INVALID_ARG, field components.<tag>", "[logging]")
{
    logging_settings_t s = fresh();
    const char *field = NULL;

    logging_core_set_component(&s, "ziggurat", ESP_LOG_DEBUG);
    s.components[0].level = (esp_log_level_t) 7;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, logging_core_validate(&s, &field));
    TEST_ASSERT_EQUAL_STRING("components.ziggurat", field);
}

TEST_CASE("syslog target that does not parse: ESP_ERR_INVALID_ARG, field channels.syslog.target", "[logging]")
{
    logging_settings_t s = fresh();
    const char *field = NULL;

    strcpy(s.syslog_target, "host:0");
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, logging_core_validate(&s, &field));
    TEST_ASSERT_EQUAL_STRING("channels.syslog.target", field);
}

TEST_CASE("syslog level above off with an empty target: ESP_ERR_INVALID_ARG, field channels.syslog.target", "[logging]")
{
    logging_settings_t s = fresh();
    const char *field = NULL;

    s.channel[LOGGING_CH_SYSLOG] = ESP_LOG_ERROR;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, logging_core_validate(&s, &field));
    TEST_ASSERT_EQUAL_STRING("channels.syslog.target", field);
}

TEST_CASE("component named \"*\": ESP_ERR_INVALID_ARG, field components.*", "[logging]")
{
    logging_settings_t s = fresh();
    const char *field = NULL;

    strcpy(s.components[0].tag, "*");
    s.components[0].level = ESP_LOG_DEBUG;
    s.n_components = 1;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, logging_core_validate(&s, &field));
    TEST_ASSERT_EQUAL_STRING("components.*", field);
}

TEST_CASE("duplicate component tag: ESP_ERR_INVALID_ARG, field components.<tag>", "[logging]")
{
    logging_settings_t s = fresh();
    const char *field = NULL;

    logging_core_set_component(&s, "wifi", ESP_LOG_WARN);
    logging_core_set_component(&s, "ziggurat", ESP_LOG_DEBUG);
    s.components[2] = s.components[1];
    s.components[2].level = ESP_LOG_ERROR;
    s.n_components = 3;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, logging_core_validate(&s, &field));
    TEST_ASSERT_EQUAL_STRING("components.ziggurat", field);
}

/* ---- logging_tagset_add / logging_tagset_list ---- */

TEST_CASE("new tag: remembered, listed in insertion order", "[logging]")
{
    logging_tagset_t set = {0};
    const char *out[4];

    TEST_ASSERT_TRUE(logging_tagset_add(&set, "wifi"));
    TEST_ASSERT_TRUE(logging_tagset_add(&set, "ziggurat"));
    TEST_ASSERT_EQUAL(2, logging_tagset_list(&set, out, 4));
    TEST_ASSERT_EQUAL_STRING("wifi", out[0]);
    TEST_ASSERT_EQUAL_STRING("ziggurat", out[1]);
    TEST_ASSERT_EQUAL(1, logging_tagset_list(&set, out, 1));
}

TEST_CASE("known tag: no duplicate", "[logging]")
{
    logging_tagset_t set = {0};
    const char *out[4];

    TEST_ASSERT_TRUE(logging_tagset_add(&set, "wifi"));
    TEST_ASSERT_FALSE(logging_tagset_add(&set, "wifi"));
    TEST_ASSERT_EQUAL(1, logging_tagset_list(&set, out, 4));
}

TEST_CASE("65th tag: ignored, returns false", "[logging]")
{
    logging_tagset_t set = {0};
    const char *out[LOGGING_KNOWN_TAGS_MAX + 1];
    char tag[8];

    for (int i = 0; i < LOGGING_KNOWN_TAGS_MAX; i++) {
        snprintf(tag, sizeof(tag), "t%d", i);
        TEST_ASSERT_TRUE(logging_tagset_add(&set, tag));
    }
    TEST_ASSERT_FALSE(logging_tagset_add(&set, "one_more"));
    TEST_ASSERT_EQUAL(LOGGING_KNOWN_TAGS_MAX, logging_tagset_list(&set, out, LOGGING_KNOWN_TAGS_MAX + 1));
}

TEST_CASE("listed tags: pointers and text unchanged by later adds (lock-free readers)", "[logging]")
{
    logging_tagset_t set = {0};
    const char *before[2];
    const char *after[3];

    TEST_ASSERT_TRUE(logging_tagset_add(&set, "wifi"));
    TEST_ASSERT_TRUE(logging_tagset_add(&set, "eth"));
    TEST_ASSERT_EQUAL(2, logging_tagset_list(&set, before, 2));
    TEST_ASSERT_TRUE(logging_tagset_add(&set, "zigbee"));
    TEST_ASSERT_FALSE(logging_tagset_add(&set, "wifi"));
    TEST_ASSERT_EQUAL(3, logging_tagset_list(&set, after, 3));
    TEST_ASSERT_EQUAL_PTR(before[0], after[0]);
    TEST_ASSERT_EQUAL_PTR(before[1], after[1]);
    TEST_ASSERT_EQUAL_STRING("wifi", before[0]);
    TEST_ASSERT_EQUAL_STRING("eth", before[1]);
    TEST_ASSERT_EQUAL_STRING("zigbee", after[2]);
}

TEST_CASE("empty, 24-byte or quoted tag: ignored, returns false", "[logging]")
{
    logging_tagset_t set = {0};

    TEST_ASSERT_FALSE(logging_tagset_add(&set, ""));
    TEST_ASSERT_FALSE(logging_tagset_add(&set, "123456789012345678901234"));
    TEST_ASSERT_FALSE(logging_tagset_add(&set, "a\"b"));
    TEST_ASSERT_FALSE(logging_tagset_add(&set, "a b"));
    TEST_ASSERT_EQUAL(0, set.count);
}

/* ---- logging_core_levels / logging_core_nonuart_max ---- */

static logging_levels_t levels(esp_log_level_t uart, esp_log_level_t jtag, esp_log_level_t syslog,
                               esp_log_level_t viewers)
{
    logging_settings_t s = fresh();

    s.channel[LOGGING_CH_UART] = uart;
    s.channel[LOGGING_CH_JTAG] = jtag;
    s.channel[LOGGING_CH_SYSLOG] = syslog;
    return logging_core_levels(&s, viewers);
}

TEST_CASE("levels of UART info, JTAG debug, syslog warning, viewers verbose: packed into one word", "[logging]")
{
    logging_levels_t lv = levels(ESP_LOG_INFO, ESP_LOG_DEBUG, ESP_LOG_WARN, ESP_LOG_VERBOSE);

    TEST_ASSERT_EQUAL(4, sizeof(lv.word));
    TEST_ASSERT_EQUAL(ESP_LOG_INFO, lv.channel[LOGGING_CH_UART]);
    TEST_ASSERT_EQUAL(ESP_LOG_DEBUG, lv.channel[LOGGING_CH_JTAG]);
    TEST_ASSERT_EQUAL(ESP_LOG_WARN, lv.channel[LOGGING_CH_SYSLOG]);
    TEST_ASSERT_EQUAL(ESP_LOG_VERBOSE, lv.viewers);
}

TEST_CASE("UART verbose, JTAG warning, syslog error, viewers off: nonuart_max is warning", "[logging]")
{
    TEST_ASSERT_EQUAL(ESP_LOG_WARN, logging_core_nonuart_max(levels(ESP_LOG_VERBOSE, ESP_LOG_WARN, ESP_LOG_ERROR,
                                                                    ESP_LOG_NONE), "radio_mux"));
    TEST_ASSERT_EQUAL(ESP_LOG_DEBUG, logging_core_nonuart_max(levels(ESP_LOG_VERBOSE, ESP_LOG_NONE, ESP_LOG_NONE,
                                                                     ESP_LOG_DEBUG), "radio_mux"));
}

TEST_CASE("network-path tag, syslog and viewers at debug: nonuart_max is info unless JTAG wants more", "[logging]")
{
    logging_levels_t lv = levels(ESP_LOG_INFO, ESP_LOG_NONE, ESP_LOG_DEBUG, ESP_LOG_VERBOSE);

    TEST_ASSERT_EQUAL(ESP_LOG_INFO, logging_core_nonuart_max(lv, "esp.emac"));
    TEST_ASSERT_EQUAL(ESP_LOG_VERBOSE, logging_core_nonuart_max(lv, "radio_mux"));
    TEST_ASSERT_EQUAL(ESP_LOG_VERBOSE, logging_core_nonuart_max(lv, NULL));
    lv.channel[LOGGING_CH_JTAG] = ESP_LOG_DEBUG;
    TEST_ASSERT_EQUAL(ESP_LOG_DEBUG, logging_core_nonuart_max(lv, "esp.emac"));
}

/* ---- logging_core_jtag_gate ---- */

TEST_CASE("JTAG gate without a USB host: the JTAG level is off, the others unchanged", "[logging]")
{
    logging_levels_t lv = logging_core_jtag_gate(levels(ESP_LOG_INFO, ESP_LOG_DEBUG, ESP_LOG_WARN, ESP_LOG_ERROR),
                                                 false);

    TEST_ASSERT_EQUAL(ESP_LOG_NONE, lv.channel[LOGGING_CH_JTAG]);
    TEST_ASSERT_EQUAL(ESP_LOG_INFO, lv.channel[LOGGING_CH_UART]);
    TEST_ASSERT_EQUAL(ESP_LOG_WARN, lv.channel[LOGGING_CH_SYSLOG]);
    TEST_ASSERT_EQUAL(ESP_LOG_ERROR, lv.viewers);
    TEST_ASSERT_EQUAL(ESP_LOG_WARN, logging_core_nonuart_max(lv, "radio_mux"));
}

TEST_CASE("JTAG gate with a USB host: levels unchanged", "[logging]")
{
    logging_levels_t in = levels(ESP_LOG_INFO, ESP_LOG_DEBUG, ESP_LOG_WARN, ESP_LOG_ERROR);

    TEST_ASSERT_EQUAL_HEX32(in.word, logging_core_jtag_gate(in, true).word);
}

TEST_CASE("JTAG-only debug line without a USB host: not routed to the ring", "[logging]")
{
    logging_levels_t lv = logging_core_jtag_gate(levels(ESP_LOG_INFO, ESP_LOG_DEBUG, ESP_LOG_NONE, ESP_LOG_NONE), false);

    TEST_ASSERT_EQUAL(0, logging_capture_route(ESP_LOG_DEBUG, ESP_LOG_INFO, logging_core_nonuart_max(lv, "x"),
                                               ESP_LOG_DEBUG, false, false));
    TEST_ASSERT_EQUAL(0, logging_core_wanting(ESP_LOG_DEBUG, "x", lv));
}

/* ---- logging_capture_route ---- */

#define ROUTE_BOTH (LOGGING_ROUTE_UART | LOGGING_ROUTE_RING)

TEST_CASE("line at or below the UART level: written through ESP-IDF's own path", "[logging]")
{
    TEST_ASSERT_EQUAL(LOGGING_ROUTE_UART,
                      logging_capture_route(ESP_LOG_INFO, ESP_LOG_INFO, ESP_LOG_NONE, ESP_LOG_INFO, false, false));
    TEST_ASSERT_EQUAL(LOGGING_ROUTE_UART,
                      logging_capture_route(ESP_LOG_ERROR, ESP_LOG_INFO, ESP_LOG_NONE, ESP_LOG_INFO, false, false));
    TEST_ASSERT_EQUAL(0, logging_capture_route(ESP_LOG_DEBUG, ESP_LOG_INFO, ESP_LOG_NONE, ESP_LOG_DEBUG, false, false));
}

TEST_CASE("line no non-UART channel wants: not formatted, ring untouched", "[logging]")
{
    TEST_ASSERT_EQUAL(0, logging_capture_route(ESP_LOG_DEBUG, ESP_LOG_INFO, ESP_LOG_INFO, ESP_LOG_VERBOSE, false,
                                               false));
    TEST_ASSERT_EQUAL(0, logging_capture_route(ESP_LOG_ERROR, ESP_LOG_NONE, ESP_LOG_NONE, ESP_LOG_VERBOSE, false,
                                               false));
}

TEST_CASE("line some non-UART channel wants: one ring record", "[logging]")
{
    TEST_ASSERT_EQUAL(LOGGING_ROUTE_RING,
                      logging_capture_route(ESP_LOG_DEBUG, ESP_LOG_INFO, ESP_LOG_DEBUG, ESP_LOG_DEBUG, false, false));
    TEST_ASSERT_EQUAL(ROUTE_BOTH,
                      logging_capture_route(ESP_LOG_INFO, ESP_LOG_INFO, ESP_LOG_DEBUG, ESP_LOG_INFO, false, false));
}

TEST_CASE("line above its tag level: ring untouched", "[logging]")
{
    TEST_ASSERT_EQUAL(0, logging_capture_route(ESP_LOG_DEBUG, ESP_LOG_NONE, ESP_LOG_VERBOSE, ESP_LOG_INFO, false,
                                               false));
}

TEST_CASE("line at level off: not routed anywhere", "[logging]")
{
    TEST_ASSERT_EQUAL(0, logging_capture_route(ESP_LOG_NONE, ESP_LOG_VERBOSE, ESP_LOG_VERBOSE, ESP_LOG_VERBOSE, false,
                                               false));
}

TEST_CASE("in an ISR or before logging_init: passed straight to ESP-IDF, ring untouched", "[logging]")
{
    TEST_ASSERT_EQUAL(LOGGING_ROUTE_UART,
                      logging_capture_route(ESP_LOG_DEBUG, ESP_LOG_INFO, ESP_LOG_VERBOSE, ESP_LOG_VERBOSE, false, true));
    TEST_ASSERT_EQUAL(LOGGING_ROUTE_UART,
                      logging_capture_route(ESP_LOG_INFO, ESP_LOG_INFO, ESP_LOG_VERBOSE, ESP_LOG_VERBOSE, true, true));
}

TEST_CASE("task inside logging_sink_enter/leave: UART only, ring untouched", "[logging]")
{
    TEST_ASSERT_EQUAL(LOGGING_ROUTE_UART,
                      logging_capture_route(ESP_LOG_INFO, ESP_LOG_INFO, ESP_LOG_DEBUG, ESP_LOG_DEBUG, true, false));
    TEST_ASSERT_EQUAL(0, logging_capture_route(ESP_LOG_DEBUG, ESP_LOG_INFO, ESP_LOG_DEBUG, ESP_LOG_DEBUG, true, false));
}

TEST_CASE("full ring: each wanting channel counted, UART never", "[logging]")
{
    logging_levels_t lv = levels(ESP_LOG_VERBOSE, ESP_LOG_DEBUG, ESP_LOG_WARN, ESP_LOG_INFO);

    TEST_ASSERT_EQUAL(1u << LOGGING_CH_JTAG, logging_core_wanting(ESP_LOG_DEBUG, "radio_mux", lv));
    TEST_ASSERT_EQUAL((1u << LOGGING_CH_JTAG) | LOGGING_WANT_LIVE, logging_core_wanting(ESP_LOG_INFO, "radio_mux", lv));
    TEST_ASSERT_EQUAL((1u << LOGGING_CH_JTAG) | (1u << LOGGING_CH_SYSLOG) | LOGGING_WANT_LIVE,
                      logging_core_wanting(ESP_LOG_ERROR, "radio_mux", lv));
    TEST_ASSERT_EQUAL(0, logging_core_wanting(ESP_LOG_VERBOSE, "radio_mux", lv));
}

TEST_CASE("full ring, debug line of a network-path tag: counted for JTAG only, not syslog or viewers", "[logging]")
{
    logging_levels_t lv = levels(ESP_LOG_INFO, ESP_LOG_DEBUG, ESP_LOG_DEBUG, ESP_LOG_DEBUG);

    TEST_ASSERT_EQUAL(1u << LOGGING_CH_JTAG, logging_core_wanting(ESP_LOG_DEBUG, "httpd_txrx", lv));
    TEST_ASSERT_EQUAL((1u << LOGGING_CH_JTAG) | (1u << LOGGING_CH_SYSLOG) | LOGGING_WANT_LIVE,
                      logging_core_wanting(ESP_LOG_INFO, "httpd_txrx", lv));
    TEST_ASSERT_EQUAL((1u << LOGGING_CH_JTAG) | (1u << LOGGING_CH_SYSLOG) | LOGGING_WANT_LIVE,
                      logging_core_wanting(ESP_LOG_DEBUG, "radio_mux", lv));
}

/* ---- logging_core_net_cap (Rulings R13, R14: no feedback loop through the network path's own debug lines) ---- */

static const char *const NET_TAGS[] = {
    "httpd", "httpd_sess", "httpd_txrx", "httpd_ws", "httpd_parse", "httpd_uri", "esp.emac", "esp.emac.dma",
    "esp_eth", "esp_eth.netif.netif_glue", "esp_netif_lwip", "esp_netif_handlers", "esp_netif_br_glue", "lwip",
    "lwip_arch", "vfs_calls",
};

TEST_CASE("net cap on a network-path tag: debug or verbose capped at info", "[logging]")
{
    for (size_t i = 0; i < sizeof(NET_TAGS) / sizeof(NET_TAGS[0]); i++) {
        TEST_ASSERT_TRUE_MESSAGE(logging_core_net_path_tag(NET_TAGS[i]), NET_TAGS[i]);
        TEST_ASSERT_EQUAL_MESSAGE(ESP_LOG_INFO, logging_core_net_cap(NET_TAGS[i], ESP_LOG_DEBUG), NET_TAGS[i]);
        TEST_ASSERT_EQUAL_MESSAGE(ESP_LOG_INFO, logging_core_net_cap(NET_TAGS[i], ESP_LOG_VERBOSE), NET_TAGS[i]);
    }
}

TEST_CASE("net cap on a network-path tag: info or below kept", "[logging]")
{
    TEST_ASSERT_EQUAL(ESP_LOG_INFO, logging_core_net_cap("httpd_ws", ESP_LOG_INFO));
    TEST_ASSERT_EQUAL(ESP_LOG_WARN, logging_core_net_cap("esp.emac", ESP_LOG_WARN));
    TEST_ASSERT_EQUAL(ESP_LOG_NONE, logging_core_net_cap("lwip", ESP_LOG_NONE));
}

TEST_CASE("net cap on any other tag: the level as it is", "[logging]")
{
    const char *tags[] = {"radio_mux", "esp_http_server", "http", "", "esp", "eth_phy_802_3", "OPENTHREAD", "lw",
                          "mdns", "esp-netif_lwip-ppp"
                         };

    for (size_t i = 0; i < sizeof(tags) / sizeof(tags[0]); i++) {
        TEST_ASSERT_FALSE_MESSAGE(logging_core_net_path_tag(tags[i]), tags[i]);
        TEST_ASSERT_EQUAL_MESSAGE(ESP_LOG_VERBOSE, logging_core_net_cap(tags[i], ESP_LOG_VERBOSE), tags[i]);
    }
    TEST_ASSERT_FALSE(logging_core_net_path_tag(NULL));
    TEST_ASSERT_EQUAL(ESP_LOG_DEBUG, logging_core_net_cap(NULL, ESP_LOG_DEBUG));
}

/* ---- logging_core_above, logging_core_clamp ---- */

TEST_CASE("settings at or below the ceiling: above returns ESP_OK, clamp changes nothing", "[logging]")
{
    logging_settings_t s;
    logging_settings_t before;
    const char *field = NULL;

    logging_core_defaults(&s);
    logging_core_set_component(&s, "ziggurat", ESP_LOG_INFO);
    before = s;
    TEST_ASSERT_EQUAL(ESP_OK, logging_core_above(&s, ESP_LOG_INFO, &field));
    TEST_ASSERT_NULL(field);
    TEST_ASSERT_FALSE(logging_core_clamp(&s, ESP_LOG_INFO));
    TEST_ASSERT_EQUAL_MEMORY(&before, &s, sizeof(s));
}

TEST_CASE("a level above the ceiling: above names channels.<name>.level, components.* or components.<tag>",
          "[logging]")
{
    logging_settings_t s;
    const char *field = NULL;

    logging_core_defaults(&s);
    s.channel[LOGGING_CH_JTAG] = ESP_LOG_DEBUG;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, logging_core_above(&s, ESP_LOG_INFO, &field));
    TEST_ASSERT_EQUAL_STRING("channels.jtag.level", field);

    logging_core_defaults(&s);
    s.default_level = ESP_LOG_VERBOSE;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, logging_core_above(&s, ESP_LOG_INFO, &field));
    TEST_ASSERT_EQUAL_STRING("components.*", field);

    logging_core_defaults(&s);
    logging_core_set_component(&s, "OPENTHREAD", ESP_LOG_WARN);
    logging_core_set_component(&s, "ziggurat", ESP_LOG_DEBUG);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, logging_core_above(&s, ESP_LOG_INFO, &field));
    TEST_ASSERT_EQUAL_STRING("components.ziggurat", field);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, logging_core_above(&s, ESP_LOG_INFO, NULL));
    TEST_ASSERT_EQUAL(ESP_OK, logging_core_above(&s, ESP_LOG_VERBOSE, NULL));
}

TEST_CASE("levels above the ceiling: clamp lowers each to the ceiling, the rest kept, returns true", "[logging]")
{
    logging_settings_t s;

    logging_core_defaults(&s);
    s.channel[LOGGING_CH_UART] = ESP_LOG_VERBOSE;
    s.default_level = ESP_LOG_DEBUG;
    logging_core_set_component(&s, "OPENTHREAD", ESP_LOG_WARN);
    logging_core_set_component(&s, "ziggurat", ESP_LOG_DEBUG);
    TEST_ASSERT_TRUE(logging_core_clamp(&s, ESP_LOG_INFO));
    TEST_ASSERT_EQUAL(ESP_LOG_INFO, s.channel[LOGGING_CH_UART]);
    TEST_ASSERT_EQUAL(ESP_LOG_INFO, s.channel[LOGGING_CH_JTAG]);
    TEST_ASSERT_EQUAL(ESP_LOG_NONE, s.channel[LOGGING_CH_SYSLOG]);
    TEST_ASSERT_EQUAL(ESP_LOG_INFO, s.default_level);
    TEST_ASSERT_EQUAL(ESP_LOG_WARN, s.components[0].level);
    TEST_ASSERT_EQUAL(ESP_LOG_INFO, s.components[1].level);
}

/* ---- logging_core_passthrough_uart ---- */

TEST_CASE("pass-through line before the levels are published: goes to ESP-IDF's writer whatever its level",
          "[logging]")
{
    logging_levels_t lv = {.word = 0};

    TEST_ASSERT_TRUE(logging_core_passthrough_uart(ESP_LOG_VERBOSE, lv, false));
    TEST_ASSERT_TRUE(logging_core_passthrough_uart(ESP_LOG_ERROR, lv, false));
}

TEST_CASE("pass-through line once published, UART info and a verbose viewer: verbose dropped, info and error kept",
          "[logging]")
{
    logging_levels_t lv = {.word = 0};

    lv.channel[LOGGING_CH_UART] = ESP_LOG_INFO;
    lv.viewers = ESP_LOG_VERBOSE;
    TEST_ASSERT_FALSE(logging_core_passthrough_uart(ESP_LOG_VERBOSE, lv, true));
    TEST_ASSERT_FALSE(logging_core_passthrough_uart(ESP_LOG_DEBUG, lv, true));
    TEST_ASSERT_TRUE(logging_core_passthrough_uart(ESP_LOG_INFO, lv, true));
    TEST_ASSERT_TRUE(logging_core_passthrough_uart(ESP_LOG_ERROR, lv, true));
}

TEST_CASE("pass-through line once published with the UART off: dropped", "[logging]")
{
    logging_levels_t lv = {.word = 0};

    TEST_ASSERT_FALSE(logging_core_passthrough_uart(ESP_LOG_ERROR, lv, true));
}

/* ---- the default (`*`) at most Debug ---- */

TEST_CASE("default at verbose in a test build (ceiling verbose): above names components.*, clamp lowers it to debug",
          "[logging]")
{
    logging_settings_t s;
    const char *field = NULL;

    logging_core_defaults(&s);
    s.default_level = ESP_LOG_VERBOSE;
    logging_core_set_component(&s, "ziggurat", ESP_LOG_VERBOSE);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, logging_core_above(&s, ESP_LOG_VERBOSE, &field));
    TEST_ASSERT_EQUAL_STRING("components.*", field);
    TEST_ASSERT_TRUE(logging_core_clamp(&s, ESP_LOG_VERBOSE));
    TEST_ASSERT_EQUAL(ESP_LOG_DEBUG, s.default_level);
    TEST_ASSERT_EQUAL(ESP_LOG_VERBOSE, s.components[0].level); // a named component keeps verbose
    TEST_ASSERT_EQUAL(ESP_OK, logging_core_above(&s, ESP_LOG_VERBOSE, NULL));
}

TEST_CASE("default's highest level: debug under a verbose ceiling, the ceiling below it", "[logging]")
{
    TEST_ASSERT_EQUAL(ESP_LOG_DEBUG, logging_core_default_max(ESP_LOG_VERBOSE));
    TEST_ASSERT_EQUAL(ESP_LOG_DEBUG, logging_core_default_max(ESP_LOG_DEBUG));
    TEST_ASSERT_EQUAL(ESP_LOG_INFO, logging_core_default_max(ESP_LOG_INFO));
}
