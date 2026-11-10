#include <stdio.h>
#include <string.h>
#include "unity.h"
#include "logging.h"
#include "logging_core.h"
#include "logging_json.h"

static logging_settings_t defaults(void)
{
    logging_settings_t s;

    logging_core_defaults(&s);
    return s;
}

/* ---- GET /device/logging ---- */

TEST_CASE("Logging GET on defaults: every channel, the default component, empty lists, zero counters", "[device_api]")
{
    logging_settings_t s = defaults();
    logging_status_t st = {0};
    char buf[1024];

    TEST_ASSERT_GREATER_THAN(0, device_api_logging_json(buf, sizeof(buf), &s, &st, NULL, 0));
    TEST_ASSERT_EQUAL_STRING("{\"channels\":{\"uart\":{\"level\":\"info\"},"
                             "\"jtag\":{\"level\":\"info\",\"host\":false},"
                             "\"syslog\":{\"level\":\"off\",\"target\":\"\",\"state\":\"off\"}},"
                             "\"components\":{\"*\":\"info\"},\"known_tags\":[],\"viewers\":0,"
                             "\"counters\":{\"uart\":{\"sent\":0,\"dropped\":0},\"jtag\":{\"sent\":0,\"dropped\":0},"
                             "\"syslog\":{\"sent\":0,\"dropped\":0},\"live\":{\"sent\":0,\"dropped\":0}}}",
                             buf);
}

TEST_CASE("Logging GET with settings and status: overrides, known tags, state, viewers and counters", "[device_api]")
{
    logging_settings_t s = defaults();
    logging_status_t st = {
        .jtag_host = true, .syslog = LOGGING_SYSLOG_SENDING, .viewers = 2,
        .sent = {10, 20, 30}, .dropped = {0, 4, 5}, .live_sent = 7, .live_dropped = 3,
    };
    const char *known[] = {"OPENTHREAD", "ziggurat"};
    char buf[1024];

    s.channel[LOGGING_CH_SYSLOG] = ESP_LOG_WARN;
    strcpy(s.syslog_target, "192.168.1.10:514");
    s.default_level = ESP_LOG_WARN;
    logging_core_set_component(&s, "ziggurat", ESP_LOG_DEBUG);
    logging_core_set_component(&s, "OPENTHREAD", ESP_LOG_INFO);
    TEST_ASSERT_GREATER_THAN(0, device_api_logging_json(buf, sizeof(buf), &s, &st, known, 2));
    TEST_ASSERT_EQUAL_STRING("{\"channels\":{\"uart\":{\"level\":\"info\"},"
                             "\"jtag\":{\"level\":\"info\",\"host\":true},"
                             "\"syslog\":{\"level\":\"warning\",\"target\":\"192.168.1.10:514\",\"state\":\"sending\"}},"
                             "\"components\":{\"*\":\"warning\",\"ziggurat\":\"debug\",\"OPENTHREAD\":\"info\"},"
                             "\"known_tags\":[\"OPENTHREAD\",\"ziggurat\"],\"viewers\":2,"
                             "\"counters\":{\"uart\":{\"sent\":10,\"dropped\":0},\"jtag\":{\"sent\":20,\"dropped\":4},"
                             "\"syslog\":{\"sent\":30,\"dropped\":5},\"live\":{\"sent\":7,\"dropped\":3}}}",
                             buf);
}

TEST_CASE("Logging GET syslog states: resolving and unresolved by name", "[device_api]")
{
    logging_settings_t s = defaults();
    logging_status_t st = {.syslog = LOGGING_SYSLOG_RESOLVING};
    char buf[1024];

    device_api_logging_json(buf, sizeof(buf), &s, &st, NULL, 0);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"state\":\"resolving\""));
    st.syslog = LOGGING_SYSLOG_UNRESOLVED;
    device_api_logging_json(buf, sizeof(buf), &s, &st, NULL, 0);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"state\":\"unresolved\""));
}

TEST_CASE("Logging GET at its largest: fits DEVICE_API_LOGGING_JSON_MAX", "[device_api]")
{
    static char buf[DEVICE_API_LOGGING_JSON_MAX];
    static char tags[LOGGING_KNOWN_TAGS_MAX][LOGGING_TAG_MAX];
    const char *known[LOGGING_KNOWN_TAGS_MAX];
    logging_settings_t s = defaults();
    logging_status_t st = {.sent = {UINT32_MAX, UINT32_MAX, UINT32_MAX}, .dropped = {UINT32_MAX, UINT32_MAX, UINT32_MAX},
                           .live_sent = UINT32_MAX, .live_dropped = UINT32_MAX, .viewers = LOGGING_VIEWERS_MAX
                          };

    memset(s.syslog_target, 'h', LOGGING_TARGET_MAX - 1);
    s.syslog_target[LOGGING_TARGET_MAX - 1] = '\0';
    for (int i = 0; i < LOGGING_KNOWN_TAGS_MAX; i++) {
        snprintf(tags[i], LOGGING_TAG_MAX, "tag_%02d_xxxxxxxxxxxxxxx", i);
        known[i] = tags[i];
        if (i < LOGGING_COMPONENTS_MAX) {
            TEST_ASSERT_EQUAL(ESP_OK, logging_core_set_component(&s, tags[i], ESP_LOG_VERBOSE));
        }
    }
    TEST_ASSERT_GREATER_THAN(0, device_api_logging_json(buf, sizeof(buf), &s, &st, known, LOGGING_KNOWN_TAGS_MAX));
}

TEST_CASE("Logging GET into a small buffer: -1", "[device_api]")
{
    logging_settings_t s = defaults();
    logging_status_t st = {0};
    char buf[64];

    TEST_ASSERT_EQUAL(-1, device_api_logging_json(buf, sizeof(buf), &s, &st, NULL, 0));
}

/* ---- PUT /device/logging ---- */

TEST_CASE("Logging PUT on a valid subset: only those fields change", "[device_api]")
{
    logging_settings_t s = defaults();
    logging_settings_t want = defaults();
    const char *field = NULL;

    logging_core_set_component(&s, "ziggurat", ESP_LOG_DEBUG);
    logging_core_set_component(&want, "ziggurat", ESP_LOG_DEBUG);
    want.channel[LOGGING_CH_SYSLOG] = ESP_LOG_WARN;
    strcpy(want.syslog_target, "10.0.0.2");
    TEST_ASSERT_EQUAL(ESP_OK, device_api_logging_parse(
                          "{\"channels\":{\"syslog\":{\"level\":\"warning\",\"target\":\"10.0.0.2\"}}}", &s, &field));
    TEST_ASSERT_EQUAL_MEMORY(&want, &s, sizeof(s));
}

TEST_CASE("Logging PUT on a valid subset: a channel level alone", "[device_api]")
{
    logging_settings_t s = defaults();
    const char *field = NULL;

    TEST_ASSERT_EQUAL(ESP_OK, device_api_logging_parse("{\"channels\":{\"jtag\":{\"level\":\"off\"}}}", &s, &field));
    TEST_ASSERT_EQUAL(ESP_LOG_NONE, s.channel[LOGGING_CH_JTAG]);
    TEST_ASSERT_EQUAL(ESP_LOG_INFO, s.channel[LOGGING_CH_UART]);
}

TEST_CASE("Logging PUT on a valid subset: an empty object changes nothing", "[device_api]")
{
    logging_settings_t s = defaults();
    logging_settings_t want = defaults();

    TEST_ASSERT_EQUAL(ESP_OK, device_api_logging_parse("{}", &s, NULL));
    TEST_ASSERT_EQUAL_MEMORY(&want, &s, sizeof(s));
}

TEST_CASE("Logging PUT on a valid subset: components object replaces the override set", "[device_api]")
{
    logging_settings_t s = defaults();

    logging_core_set_component(&s, "ziggurat", ESP_LOG_DEBUG);
    logging_core_set_component(&s, "eth", ESP_LOG_ERROR);
    TEST_ASSERT_EQUAL(ESP_OK, device_api_logging_parse("{\"components\":{\"OPENTHREAD\":\"verbose\"}}", &s, NULL));
    TEST_ASSERT_EQUAL(1, s.n_components);
    TEST_ASSERT_EQUAL_STRING("OPENTHREAD", s.components[0].tag);
    TEST_ASSERT_EQUAL(ESP_LOG_VERBOSE, s.components[0].level);
    TEST_ASSERT_EQUAL(ESP_LOG_INFO, s.default_level); // no "*": unchanged
}

TEST_CASE("Logging PUT on a valid subset: \"*\" sets default_level", "[device_api]")
{
    logging_settings_t s = defaults();

    logging_core_set_component(&s, "ziggurat", ESP_LOG_DEBUG);
    TEST_ASSERT_EQUAL(ESP_OK, device_api_logging_parse("{\"components\":{\"*\":\"debug\"}}", &s, NULL));
    TEST_ASSERT_EQUAL(ESP_LOG_DEBUG, s.default_level);
    TEST_ASSERT_EQUAL(0, s.n_components);
}

TEST_CASE("Logging PUT on a valid subset: the GET body's read-only fields are ignored", "[device_api]")
{
    logging_settings_t s = defaults();
    logging_settings_t want = defaults();

    TEST_ASSERT_EQUAL(ESP_OK, device_api_logging_parse(
                          "{\"channels\":{\"jtag\":{\"level\":\"info\",\"host\":true},"
                          "\"syslog\":{\"level\":\"off\",\"target\":\"\",\"state\":\"off\"}},"
                          "\"known_tags\":[\"x\"],\"viewers\":1,\"counters\":{}}",
                          &s, NULL));
    TEST_ASSERT_EQUAL_MEMORY(&want, &s, sizeof(s));
}

/** Parse `body` over defaults: INVALID_ARG, `field` named, settings untouched. */
static void assert_bad(const char *body, const char *field)
{
    logging_settings_t s = defaults();
    logging_settings_t want = defaults();
    const char *got = NULL;

    TEST_ASSERT_EQUAL_MESSAGE(ESP_ERR_INVALID_ARG, device_api_logging_parse(body, &s, &got), body);
    TEST_ASSERT_NOT_NULL_MESSAGE(got, body);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(field, got, body);
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(&want, &s, sizeof(s), body);
}

TEST_CASE("Logging PUT on a bad level: invalid_request with the level's field", "[device_api]")
{
    assert_bad("{\"channels\":{\"uart\":{\"level\":\"loud\"}}}", "channels.uart.level");
    assert_bad("{\"channels\":{\"syslog\":{\"level\":4}}}", "channels.syslog.level");
    assert_bad("{\"channels\":{\"jtag\":{\"level\":\"WARN\"}}}", "channels.jtag.level");
    assert_bad("{\"components\":{\"*\":\"warn\"}}", "components.*");
    assert_bad("{\"components\":{\"ziggurat\":\"loud\"}}", "components.ziggurat");
    assert_bad("{\"components\":{\"ziggurat\":3}}", "components.ziggurat");
}

TEST_CASE("Logging PUT on a bad target: invalid_request with channels.syslog.target", "[device_api]")
{
    assert_bad("{\"channels\":{\"syslog\":{\"target\":\"host:0\"}}}", "channels.syslog.target");
    assert_bad("{\"channels\":{\"syslog\":{\"target\":\"[fd00::1\"}}}", "channels.syslog.target");
    assert_bad("{\"channels\":{\"syslog\":{\"target\":5}}}", "channels.syslog.target");
}

TEST_CASE("Logging PUT on a target too long: invalid_request with channels.syslog.target", "[device_api]")
{
    char body[LOGGING_TARGET_MAX + 64];
    char host[LOGGING_TARGET_MAX + 1];

    memset(host, 'a', LOGGING_TARGET_MAX);
    host[LOGGING_TARGET_MAX] = '\0';
    snprintf(body, sizeof(body), "{\"channels\":{\"syslog\":{\"target\":\"%s\"}}}", host);
    assert_bad(body, "channels.syslog.target");
}

TEST_CASE("Logging PUT on not an object: invalid_request with the field", "[device_api]")
{
    assert_bad("[1]", "body");
    assert_bad("{\"channels\":", "body");
    assert_bad("{\"channels\":[]}", "channels");
    assert_bad("{\"channels\":{\"uart\":\"info\"}}", "channels.uart");
    assert_bad("{\"components\":[\"ziggurat\"]}", "components");
}

TEST_CASE("Logging PUT on a bad component tag: invalid_request with the tag's field", "[device_api]")
{
    assert_bad("{\"components\":{\"\":\"info\"}}", "components.");
    assert_bad("{\"components\":{\"a_tag_of_twenty_four_chr\":\"info\"}}", "components.a_tag_of_twenty_four_chr");
}

TEST_CASE("Logging PUT on a component tag listed twice: invalid_request with the tag's field", "[device_api]")
{
    assert_bad("{\"components\":{\"ziggurat\":\"debug\",\"ziggurat\":\"info\"}}", "components.ziggurat");
    assert_bad("{\"components\":{\"*\":\"debug\",\"*\":\"info\"}}", "components.*");
}

TEST_CASE("Logging PUT with unknown top-level and channel keys: ignored, the rest applied", "[device_api]")
{
    logging_settings_t s = defaults();

    TEST_ASSERT_EQUAL(ESP_OK, device_api_logging_parse(
                          "{\"verbose\":true,\"channels\":{\"live\":{\"level\":\"debug\"},"
                          "\"uart\":{\"level\":\"warning\",\"baud\":115200}}}",
                          &s, NULL));
    TEST_ASSERT_EQUAL(ESP_LOG_WARN, s.channel[LOGGING_CH_UART]);
    TEST_ASSERT_EQUAL(ESP_LOG_INFO, s.channel[LOGGING_CH_JTAG]);
}

TEST_CASE("Logging PUT on 33 components: invalid_request with components", "[device_api]")
{
    char body[2048];
    size_t len = 0;

    len += (size_t) snprintf(body + len, sizeof(body) - len, "{\"components\":{");
    for (int i = 0; i <= LOGGING_COMPONENTS_MAX; i++) {
        len += (size_t) snprintf(body + len, sizeof(body) - len, "%s\"t%d\":\"info\"", i ? "," : "", i);
    }
    snprintf(body + len, sizeof(body) - len, "}}");
    assert_bad(body, "components");
}

TEST_CASE("Logging PUT with 32 components: accepted", "[device_api]")
{
    char body[2048];
    size_t len = 0;
    logging_settings_t s = defaults();

    len += (size_t) snprintf(body + len, sizeof(body) - len, "{\"components\":{");
    for (int i = 0; i < LOGGING_COMPONENTS_MAX; i++) {
        len += (size_t) snprintf(body + len, sizeof(body) - len, "%s\"t%d\":\"info\"", i ? "," : "", i);
    }
    snprintf(body + len, sizeof(body) - len, "}}");
    TEST_ASSERT_EQUAL(ESP_OK, device_api_logging_parse(body, &s, NULL));
    TEST_ASSERT_EQUAL(LOGGING_COMPONENTS_MAX, s.n_components);
}

/* ---- /device/logging/ws ---- */

TEST_CASE("Logs WebSocket line: \"<L> (<uptime>) <tag>: <body>\"", "[device_api]")
{
    logging_line_t l = {.uptime_ms = 813120, .level = ESP_LOG_WARN, .tag = "zigbee", .len = 15,
                        .text = "request timeoutXXX"
                       };
    char buf[DEVICE_API_LOGS_FRAME_MAX];

    TEST_ASSERT_EQUAL(34, device_api_logs_line(buf, sizeof(buf), &l));
    TEST_ASSERT_EQUAL_STRING("W (813120) zigbee: request timeout", buf);
    l.level = ESP_LOG_VERBOSE;
    device_api_logs_line(buf, sizeof(buf), &l);
    TEST_ASSERT_EQUAL('V', buf[0]);
}

TEST_CASE("Logs WebSocket line at its longest: fits DEVICE_API_LOGS_FRAME_MAX", "[device_api]")
{
    static char body[LOGGING_LINE_MAX];
    char buf[DEVICE_API_LOGS_FRAME_MAX];
    logging_line_t l = {.uptime_ms = UINT32_MAX, .level = ESP_LOG_ERROR, .len = LOGGING_LINE_MAX, .text = body};

    memset(body, 'b', sizeof(body));
    memset(l.tag, 't', LOGGING_TAG_MAX - 1);
    l.tag[LOGGING_TAG_MAX - 1] = '\0';
    TEST_ASSERT_GREATER_THAN(0, device_api_logs_line(buf, sizeof(buf), &l));
    TEST_ASSERT_EQUAL(-1, device_api_logs_line(buf, 32, &l));
}

TEST_CASE("Logs WebSocket drops for this viewer: a frame \"# dropped <n> lines\"", "[device_api]")
{
    char buf[48];

    TEST_ASSERT_GREATER_THAN(0, device_api_logs_dropped(buf, sizeof(buf), 12));
    TEST_ASSERT_EQUAL_STRING("# dropped 12 lines", buf);
}

TEST_CASE("Logs WebSocket connect with ?level=debug: the viewer's level is debug", "[device_api]")
{
    esp_log_level_t level = ESP_LOG_NONE;

    TEST_ASSERT_EQUAL(ESP_OK, device_api_logs_query_level("level=debug", &level));
    TEST_ASSERT_EQUAL(ESP_LOG_DEBUG, level);
    TEST_ASSERT_EQUAL(ESP_OK, device_api_logs_query_level("x=1&level=off", &level));
    TEST_ASSERT_EQUAL(ESP_LOG_NONE, level);
}

TEST_CASE("Logs WebSocket connect without a level: info", "[device_api]")
{
    esp_log_level_t level = ESP_LOG_NONE;

    TEST_ASSERT_EQUAL(ESP_OK, device_api_logs_query_level(NULL, &level));
    TEST_ASSERT_EQUAL(ESP_LOG_INFO, level);
    level = ESP_LOG_NONE;
    TEST_ASSERT_EQUAL(ESP_OK, device_api_logs_query_level("x=1", &level));
    TEST_ASSERT_EQUAL(ESP_LOG_INFO, level);
}

TEST_CASE("Logs WebSocket connect with a bad level: invalid, level untouched", "[device_api]")
{
    esp_log_level_t level = ESP_LOG_ERROR;

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, device_api_logs_query_level("level=loud", &level));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, device_api_logs_query_level("level=", &level));
    TEST_ASSERT_EQUAL(ESP_LOG_ERROR, level);
}

TEST_CASE("Logs WebSocket text frame {\"level\":\"warning\"}: level warning", "[device_api]")
{
    const char *frame = "{\"level\":\"warning\"}";
    esp_log_level_t level = ESP_LOG_NONE;

    TEST_ASSERT_EQUAL(ESP_OK, device_api_logs_level_frame(frame, strlen(frame), &level));
    TEST_ASSERT_EQUAL(ESP_LOG_WARN, level);
}

TEST_CASE("Logs WebSocket text frame that is not a level: invalid, level untouched", "[device_api]")
{
    const char *bad[] = {"{\"level\":\"loud\"}", "{\"level\":2}", "{}", "warning", "{\"level\":\"warning\""};
    esp_log_level_t level = ESP_LOG_ERROR;

    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        TEST_ASSERT_EQUAL_MESSAGE(ESP_ERR_INVALID_ARG, device_api_logs_level_frame(bad[i], strlen(bad[i]), &level),
                                  bad[i]);
    }
    // Not NUL-terminated: only `len` bytes count.
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, device_api_logs_level_frame("{\"level\":\"info\"}garbage", 10, &level));
    TEST_ASSERT_EQUAL(ESP_LOG_ERROR, level);
}

TEST_CASE("Logs WebSocket on a third viewer: 503 too many viewers before the upgrade", "[device_api]")
{
    TEST_ASSERT_EQUAL(2, LOGGING_VIEWERS_MAX); // Ruling R11
    TEST_ASSERT_EQUAL(503, device_api_logs_ws_refusal(LOGGING_VIEWERS_MAX));
    TEST_ASSERT_EQUAL(0, device_api_logs_ws_refusal(LOGGING_VIEWERS_MAX - 1));
    TEST_ASSERT_EQUAL_STRING("too many viewers", DEVICE_API_LOGS_TOO_MANY);
}
