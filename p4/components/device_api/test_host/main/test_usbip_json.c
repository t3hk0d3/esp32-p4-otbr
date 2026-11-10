#include <string.h>
#include "unity.h"
#include "unity_test_runner.h"
#include "usbip_json.h"

// The spec section 9 example, compact.
#define SPEC_BODY \
    "{\"enabled\":true,\"active\":true,\"pending_reboot\":false," \
    "\"state\":\"attached\",\"reason\":null," \
    "\"port\":3240,\"interface\":\"mgmt\"," \
    "\"device\":{\"busid\":\"1-1\",\"vid\":\"10c4\",\"pid\":\"ea60\",\"bcd_device\":\"0100\",\"class\":\"00\"," \
    "\"speed\":\"full\",\"manufacturer\":\"Silicon Labs\"," \
    "\"product\":\"CP2102N USB to UART Bridge Controller\"," \
    "\"serial\":\"abc123\",\"interfaces\":1,\"endpoints\":2}," \
    "\"client\":{\"addr\":\"192.168.10.20:51322\",\"since\":1762790000}," \
    "\"counters\":{\"sessions\":3,\"refused\":1,\"submits\":12000,\"completed\":11950,\"errors\":2,\"unlinks\":48," \
    "\"bytes_in\":1048576,\"bytes_out\":65536,\"sessions_ended_overload\":0}," \
    "\"memory\":{\"inflight_bytes\":4096,\"inflight_peak\":16384,\"waiting_bytes\":0,\"waiting_peak\":2048}," \
    "\"last_error\":null}"

static usbip_status_t full(void)
{
    usbip_status_t st = {0};

    st.enabled = true;
    st.active = true;
    st.state = USBIP_STATE_ATTACHED;
    st.port = 3240;
    st.iface = "mgmt";
    st.has_device = true;
    strcpy(st.dev.busid, "1-1");
    st.dev.vid = 0x10c4;
    st.dev.pid = 0xea60;
    st.dev.bcd_device = 0x0100;
    st.dev.dev_class = 0;
    st.dev.speed = 2;
    st.dev.num_ifaces = 1;
    strcpy(st.manufacturer, "Silicon Labs");
    strcpy(st.product, "CP2102N USB to UART Bridge Controller");
    strcpy(st.serial, "abc123");
    st.endpoints = 2;
    st.has_client = true;
    strcpy(st.client, "192.168.10.20:51322");
    st.client_since = 1762790000;
    st.sessions = 3;
    st.refused = 1;
    st.counters = (usbip_counters_t) {
        .submits = 12000, .completed = 11950, .errors = 2, .unlinks = 48, .overload_ends = 0,
        .bytes_in = 1048576, .bytes_out = 65536,
    };
    st.mem = (usbip_mem_stats_t) {.inflight = 4096, .inflight_peak = 16384, .waiting = 0, .waiting_peak = 2048};
    return st;
}

static char body[USBIP_JSON_MAX];

TEST_CASE("usbip_json_status full: matches the spec example byte for byte", "[device_api]")
{
    usbip_status_t st = full();
    size_t len = 0;

    TEST_ASSERT_EQUAL_INT(ESP_OK, usbip_json_status(&st, body, sizeof(body), &len));
    TEST_ASSERT_EQUAL_STRING(SPEC_BODY, body);
    TEST_ASSERT_EQUAL_UINT(strlen(SPEC_BODY), len);
}

TEST_CASE("usbip_json_status without device and client: both are null", "[device_api]")
{
    usbip_status_t st = full();
    size_t len = 0;

    st.has_device = false;
    st.has_client = false;
    st.state = USBIP_STATE_NO_DEVICE;
    TEST_ASSERT_EQUAL_INT(ESP_OK, usbip_json_status(&st, body, sizeof(body), &len));
    TEST_ASSERT_NOT_NULL(strstr(body, "\"state\":\"no_device\""));
    TEST_ASSERT_NOT_NULL(strstr(body, "\"device\":null,\"client\":null,\"counters\""));
}

TEST_CASE("usbip_json_status with reason and last_error: both are strings", "[device_api]")
{
    usbip_status_t st = full();
    size_t len = 0;

    strcpy(st.reason, "a \"hub\" is not supported");
    strcpy(st.last_error, "session ended: overload");
    TEST_ASSERT_EQUAL_INT(ESP_OK, usbip_json_status(&st, body, sizeof(body), &len));
    TEST_ASSERT_NOT_NULL(strstr(body, "\"reason\":\"a \\\"hub\\\" is not supported\""));
    TEST_ASSERT_NOT_NULL(strstr(body, "\"last_error\":\"session ended: overload\"}"));
}

TEST_CASE("usbip_json_status speed: low, full, high, anything else unknown", "[device_api]")
{
    const char *want[] = {"unknown", "low", "full", "high", "unknown"};
    const uint32_t speed[] = {0, 1, 2, 3, 4};

    for (int i = 0; i < 5; i++) {
        usbip_status_t st = full();
        size_t len;
        char needle[32];

        st.dev.speed = speed[i];
        TEST_ASSERT_EQUAL_INT(ESP_OK, usbip_json_status(&st, body, sizeof(body), &len));
        snprintf(needle, sizeof(needle), "\"speed\":\"%s\"", want[i]);
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(body, needle), needle);
    }
}

TEST_CASE("usbip_json_status hex ids: lowercase, zero padded", "[device_api]")
{
    usbip_status_t st = full();
    size_t len;

    st.dev.vid = 0x0ABC;
    st.dev.pid = 0x000f;
    st.dev.bcd_device = 0x0002;
    st.dev.dev_class = 0xEF;
    TEST_ASSERT_EQUAL_INT(ESP_OK, usbip_json_status(&st, body, sizeof(body), &len));
    TEST_ASSERT_NOT_NULL(strstr(body, "\"vid\":\"0abc\",\"pid\":\"000f\",\"bcd_device\":\"0002\",\"class\":\"ef\""));
}

TEST_CASE("usbip_json_status widest values: fits USBIP_JSON_MAX", "[device_api]")
{
    usbip_status_t st = full();
    size_t len;

    memset(st.manufacturer, '"', sizeof(st.manufacturer) - 1);
    memset(st.product, '"', sizeof(st.product) - 1);
    memset(st.serial, '\\', sizeof(st.serial) - 1);
    memset(st.reason, '"', sizeof(st.reason) - 1);
    memset(st.last_error, '"', sizeof(st.last_error) - 1);
    memset(st.client, 'x', sizeof(st.client) - 1);
    st.client_since = INT64_MAX;
    st.counters.bytes_in = UINT64_MAX;
    st.counters.bytes_out = UINT64_MAX;
    st.counters.submits = st.counters.completed = st.counters.errors = st.counters.unlinks = UINT32_MAX;
    st.sessions = st.refused = UINT32_MAX;
    TEST_ASSERT_EQUAL_INT(ESP_OK, usbip_json_status(&st, body, sizeof(body), &len));
}

TEST_CASE("usbip_json_status too small a buffer: ESP_ERR_INVALID_SIZE, NUL-terminated prefix", "[device_api]")
{
    usbip_status_t st = full();
    char small[64];
    size_t len = 0;

    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_SIZE, usbip_json_status(&st, small, sizeof(small), &len));
    TEST_ASSERT_LESS_THAN_UINT(sizeof(small), strlen(small));
    TEST_ASSERT_EQUAL_INT(0, strncmp(small, SPEC_BODY, strlen(small)));
}

static esp_err_t parse(const char *text, usbip_put_kind_t *kind, bool *enabled)
{
    *kind = USBIP_PUT_KICK;
    *enabled = false;
    return usbip_json_put_parse(text, kind, enabled);
}

TEST_CASE("usbip_json_put_parse enabled: true and false are a setting", "[device_api]")
{
    usbip_put_kind_t kind;
    bool enabled;

    TEST_ASSERT_EQUAL_INT(ESP_OK, parse("{\"enabled\": true}", &kind, &enabled));
    TEST_ASSERT_EQUAL_INT(USBIP_PUT_SET, kind);
    TEST_ASSERT_TRUE(enabled);
    TEST_ASSERT_EQUAL_INT(ESP_OK, parse(" {\"enabled\":false} ", &kind, &enabled));
    TEST_ASSERT_EQUAL_INT(USBIP_PUT_SET, kind);
    TEST_ASSERT_FALSE(enabled);
}

TEST_CASE("usbip_json_put_parse kick: true is a kick", "[device_api]")
{
    usbip_put_kind_t kind;
    bool enabled;

    TEST_ASSERT_EQUAL_INT(ESP_OK, parse("{\"kick\":true}", &kind, &enabled));
    TEST_ASSERT_EQUAL_INT(USBIP_PUT_KICK, kind);
}

TEST_CASE("usbip_json_put_parse anything else: ESP_ERR_INVALID_ARG", "[device_api]")
{
    const char *bad[] = {
        "{\"kick\":false}", "{\"kick\":1}", "{\"kick\":\"true\"}", "{\"enabled\":1}", "{\"enabled\":\"true\"}",
        "{\"enabled\":null}", "{\"enabled\":true,\"kick\":true}", "{\"enabled\":true,\"x\":1}", "{\"kick\":true,\"x\":1}",
        "{\"enabled\":true,\"enabled\":true}", "{}", "{\"x\":true}", "[]", "true", "\"enabled\"", "", "{\"enabled\":",
        "not json",
    };

    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        usbip_put_kind_t kind;
        bool enabled;

        TEST_ASSERT_EQUAL_INT_MESSAGE(ESP_ERR_INVALID_ARG, parse(bad[i], &kind, &enabled), bad[i]);
    }
    usbip_put_kind_t kind;
    bool enabled;

    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_ARG, usbip_json_put_parse(NULL, &kind, &enabled));
}
