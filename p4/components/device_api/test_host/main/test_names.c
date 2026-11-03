#include <string.h>
#include "unity.h"
#include "unity_test_runner.h"
#include "cJSON.h"
#include "device_api_priv.h"
#include "names_json.h"

static netnames_t sample(void)
{
    netnames_t n = {
        .set = {.hostname = "kitchen", .ha_name_follows_hostname = true, .http_advert = false},
        .hostname = "kitchen",
        .hostname_default = "esp-otbr-a1b2c3",
        .hostname_live = "kitchen",
        .ha_name = "kitchen",
        .dhcp_name = "esp-otbr",
        .http_advert_available = true,
    };

    return n;
}

static cJSON *render(const netnames_t *n, bool split, const netnames_result_t *res)
{
    char buf[1024];

    TEST_ASSERT_GREATER_THAN(0, device_api_names_json(buf, sizeof(buf), n, split, res));
    cJSON *json = cJSON_Parse(buf);

    TEST_ASSERT_NOT_NULL_MESSAGE(json, buf);
    return json;
}

static const char *str(const cJSON *json, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(json, key);

    TEST_ASSERT_TRUE_MESSAGE(cJSON_IsString(item), key);
    return item->valuestring;
}

static bool flag(const cJSON *json, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(json, key);

    TEST_ASSERT_TRUE_MESSAGE(cJSON_IsBool(item), key);
    return cJSON_IsTrue(item);
}

/* ---- device_api_names_json ---- */

TEST_CASE("Names GET body: every field of the API, mode single, no applied", "[device_api]")
{
    netnames_t n = sample();
    cJSON *json = render(&n, false, NULL);

    TEST_ASSERT_EQUAL_STRING("kitchen", str(json, "hostname"));
    TEST_ASSERT_EQUAL_STRING("esp-otbr-a1b2c3", str(json, "hostname_default"));
    TEST_ASSERT_EQUAL_STRING("kitchen", str(json, "hostname_live"));
    TEST_ASSERT_TRUE(flag(json, "ha_name_follows_hostname"));
    TEST_ASSERT_EQUAL_STRING("", str(json, "ha_name"));
    TEST_ASSERT_EQUAL_STRING("kitchen", str(json, "ha_name_effective"));
    TEST_ASSERT_FALSE(flag(json, "http_advert"));
    TEST_ASSERT_TRUE(flag(json, "http_advert_available"));
    TEST_ASSERT_EQUAL_STRING("", str(json, "dhcp_name"));
    TEST_ASSERT_EQUAL_STRING("esp-otbr", str(json, "dhcp_name_default"));
    TEST_ASSERT_EQUAL_STRING("single", str(json, "mode"));
    TEST_ASSERT_NULL(cJSON_GetObjectItemCaseSensitive(json, "applied"));
    TEST_ASSERT_NULL(cJSON_GetObjectItemCaseSensitive(json, "detail"));
    cJSON_Delete(json);
}

TEST_CASE("Names GET body in split mode: mode split, advert not available", "[device_api]")
{
    netnames_t n = sample();

    n.http_advert_available = false;
    cJSON *json = render(&n, true, NULL);

    TEST_ASSERT_EQUAL_STRING("split", str(json, "mode"));
    TEST_ASSERT_FALSE(flag(json, "http_advert_available"));
    cJSON_Delete(json);
}

TEST_CASE("Names body with a UTF-8 HA name and a quote: written as valid JSON", "[device_api]")
{
    netnames_t n = sample();

    n.set.ha_name_follows_hostname = false;
    strlcpy(n.set.ha_name, "\xd0\x96 \"Kitchen\"", sizeof(n.set.ha_name));
    strlcpy(n.ha_name, n.set.ha_name, sizeof(n.ha_name));
    cJSON *json = render(&n, false, NULL);

    TEST_ASSERT_EQUAL_STRING("\xd0\x96 \"Kitchen\"", str(json, "ha_name"));
    TEST_ASSERT_FALSE(flag(json, "ha_name_follows_hostname"));
    cJSON_Delete(json);
}

TEST_CASE("Names PUT result, applied: applied true, no detail", "[device_api]")
{
    netnames_t n = sample();
    netnames_result_t res = {.applied = true};
    cJSON *json = render(&n, false, &res);

    TEST_ASSERT_TRUE(flag(json, "applied"));
    TEST_ASSERT_NULL(cJSON_GetObjectItemCaseSensitive(json, "detail"));
    cJSON_Delete(json);
}

TEST_CASE("Names PUT result, not applied: applied false and the detail", "[device_api]")
{
    netnames_t n = sample();
    netnames_result_t res = {.applied = false, .detail = "hostname not applied"};
    cJSON *json = render(&n, false, &res);

    TEST_ASSERT_FALSE(flag(json, "applied"));
    TEST_ASSERT_EQUAL_STRING("hostname not applied", str(json, "detail"));
    cJSON_Delete(json);
}

TEST_CASE("Names body in a buffer too small: -1", "[device_api]")
{
    char buf[32];
    netnames_t n = sample();

    TEST_ASSERT_EQUAL(-1, device_api_names_json(buf, sizeof(buf), &n, false, NULL));
}

/* ---- device_api_names_error_json ---- */

TEST_CASE("Names validation error: invalid_config with the field and a detail naming it", "[device_api]")
{
    char buf[256];

    TEST_ASSERT_GREATER_THAN(0, device_api_names_error_json(buf, sizeof(buf), "hostname", "must not start or end with a hyphen"));
    cJSON *json = cJSON_Parse(buf);

    TEST_ASSERT_NOT_NULL(json);
    TEST_ASSERT_EQUAL_STRING("invalid_config", str(json, "error"));
    TEST_ASSERT_EQUAL_STRING("hostname", str(json, "field"));
    TEST_ASSERT_EQUAL_STRING("hostname: must not start or end with a hyphen", str(json, "detail"));
    cJSON_Delete(json);
}

/* ---- device_api_names_parse ---- */

static const char *VALID = "{\"hostname\":\"Kitchen\",\"ha_name_follows_hostname\":false,\"ha_name\":\"Kitchen OTBR\","
                           "\"http_advert\":true,\"dhcp_name\":\"router\"}";

TEST_CASE("Names parse, valid body: every field filled", "[device_api]")
{
    netnames_settings_t out;
    const char *field = NULL;

    TEST_ASSERT_EQUAL(ESP_OK, device_api_names_parse(VALID, &out, &field));
    TEST_ASSERT_EQUAL_STRING("Kitchen", out.hostname);
    TEST_ASSERT_FALSE(out.ha_name_follows_hostname);
    TEST_ASSERT_EQUAL_STRING("Kitchen OTBR", out.ha_name);
    TEST_ASSERT_TRUE(out.http_advert);
    TEST_ASSERT_EQUAL_STRING("router", out.dhcp_name);
}

static void assert_parse_fails(const char *body, const char *expected_field)
{
    netnames_settings_t out;
    const char *field = NULL;

    TEST_ASSERT_EQUAL_MESSAGE(ESP_ERR_INVALID_ARG, device_api_names_parse(body, &out, &field), body);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(expected_field, field, body);
}

TEST_CASE("Names parse, not an object: field body", "[device_api]")
{
    assert_parse_fails("not json", "body");
    assert_parse_fails("[1,2]", "body");
}

TEST_CASE("Names parse, text after the object: field body", "[device_api]")
{
    char body[512];

    snprintf(body, sizeof(body), "%s garbage", VALID);
    assert_parse_fails(body, "body");
    snprintf(body, sizeof(body), "%s%s", VALID, VALID);
    assert_parse_fails(body, "body");
}

TEST_CASE("Names parse, a missing field: that field", "[device_api]")
{
    assert_parse_fails("{\"ha_name_follows_hostname\":true,\"ha_name\":\"\",\"http_advert\":false,\"dhcp_name\":\"\"}",
                       "hostname");
    assert_parse_fails("{\"hostname\":\"\",\"ha_name_follows_hostname\":true,\"ha_name\":\"\",\"http_advert\":false}",
                       "dhcp_name");
}

TEST_CASE("Names parse, a wrong type: that field", "[device_api]")
{
    assert_parse_fails("{\"hostname\":\"\",\"ha_name_follows_hostname\":\"yes\",\"ha_name\":\"\",\"http_advert\":false,"
                       "\"dhcp_name\":\"\"}", "ha_name_follows_hostname");
    assert_parse_fails("{\"hostname\":1,\"ha_name_follows_hostname\":true,\"ha_name\":\"\",\"http_advert\":false,"
                       "\"dhcp_name\":\"\"}", "hostname");
}

TEST_CASE("Names parse, a string of 64 bytes: that field", "[device_api]")
{
    char body[256];

    snprintf(body, sizeof(body), "{\"hostname\":\"\",\"ha_name_follows_hostname\":false,\"ha_name\":\"%064d\","
             "\"http_advert\":false,\"dhcp_name\":\"\"}", 0);
    assert_parse_fails(body, "ha_name");
}
