#include <string.h>
#include "unity.h"
#include "unity_test_runner.h"
#include "br_core_mode.h"
#include "cJSON.h"
#include "device_api_priv.h"
#include "thread_json.h"

static esp_err_t parse(const char *body, device_api_thread_put_t *out, const char **field)
{
    memset(out, 0, sizeof(*out));
    return device_api_thread_parse(body, out, field);
}

static void expect_bad(const char *body, const char *want_field)
{
    device_api_thread_put_t put;
    const char *field = NULL;

    TEST_ASSERT_EQUAL_INT_MESSAGE(ESP_ERR_INVALID_ARG, parse(body, &put, &field), body);
    TEST_ASSERT_NOT_NULL_MESSAGE(field, body);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(want_field, field, body);
}

static int render(char *buf, size_t cap, const device_api_thread_t *t)
{
    return device_api_thread_json(buf, cap, t);
}

/* ---- device_api_thread_parse ---- */

TEST_CASE("Thread PUT adjustment 8: ok, has_adjustment, 8, become_leader false", "[device_api]")
{
    device_api_thread_put_t put;
    const char *field = NULL;

    TEST_ASSERT_EQUAL_INT(ESP_OK, parse("{\"leader_weight_adjustment\": 8}", &put, &field));
    TEST_ASSERT_TRUE(put.has_adjustment);
    TEST_ASSERT_EQUAL_INT8(8, put.adjustment);
    TEST_ASSERT_FALSE(put.become_leader);
}

TEST_CASE("Thread PUT adjustment 8.0: ok, 8", "[device_api]")
{
    device_api_thread_put_t put;
    const char *field = NULL;

    TEST_ASSERT_EQUAL_INT(ESP_OK, parse("{\"leader_weight_adjustment\": 8.0}", &put, &field));
    TEST_ASSERT_TRUE(put.has_adjustment);
    TEST_ASSERT_EQUAL_INT8(8, put.adjustment);
}

TEST_CASE("Thread PUT adjustment -16 and 16: ok at the limits", "[device_api]")
{
    device_api_thread_put_t put;
    const char *field = NULL;

    TEST_ASSERT_EQUAL_INT(ESP_OK, parse("{\"leader_weight_adjustment\":-16}", &put, &field));
    TEST_ASSERT_EQUAL_INT8(-16, put.adjustment);
    TEST_ASSERT_EQUAL_INT(ESP_OK, parse("{\"leader_weight_adjustment\":16}", &put, &field));
    TEST_ASSERT_EQUAL_INT8(16, put.adjustment);
}

TEST_CASE("Thread PUT become_leader true: ok, become_leader true, has_adjustment false", "[device_api]")
{
    device_api_thread_put_t put;
    const char *field = NULL;

    TEST_ASSERT_EQUAL_INT(ESP_OK, parse("{\"become_leader\":true}", &put, &field));
    TEST_ASSERT_TRUE(put.become_leader);
    TEST_ASSERT_FALSE(put.has_adjustment);
}

TEST_CASE("Thread PUT both keys: ok, both set", "[device_api]")
{
    device_api_thread_put_t put;
    const char *field = NULL;

    TEST_ASSERT_EQUAL_INT(ESP_OK, parse("{\"become_leader\":true,\"leader_weight_adjustment\":-3}", &put, &field));
    TEST_ASSERT_TRUE(put.become_leader);
    TEST_ASSERT_TRUE(put.has_adjustment);
    TEST_ASSERT_EQUAL_INT8(-3, put.adjustment);
}

TEST_CASE("Thread PUT become_leader false: ok, nothing to do", "[device_api]")
{
    device_api_thread_put_t put;
    const char *field = NULL;

    TEST_ASSERT_EQUAL_INT(ESP_OK, parse("{\"become_leader\":false}", &put, &field));
    TEST_ASSERT_FALSE(put.become_leader);
    TEST_ASSERT_FALSE(put.has_adjustment);
}

TEST_CASE("Thread PUT unknown key next to a known one: ok, ignored", "[device_api]")
{
    device_api_thread_put_t put;
    const char *field = NULL;

    TEST_ASSERT_EQUAL_INT(ESP_OK, parse("{\"x\":1,\"leader_weight_adjustment\":2}", &put, &field));
    TEST_ASSERT_TRUE(put.has_adjustment);
    TEST_ASSERT_EQUAL_INT8(2, put.adjustment);
}

TEST_CASE("Thread PUT {}: invalid, field body", "[device_api]")
{
    expect_bad("{}", "body");
}

TEST_CASE("Thread PUT {\"x\":1}: invalid, field body", "[device_api]")
{
    expect_bad("{\"x\":1}", "body");
}

TEST_CASE("Thread PUT []: invalid, field body", "[device_api]")
{
    expect_bad("[]", "body");
}

TEST_CASE("Thread PUT \"8\": invalid, field body", "[device_api]")
{
    expect_bad("\"8\"", "body");
}

TEST_CASE("Thread PUT not JSON: invalid, field body", "[device_api]")
{
    expect_bad("not json", "body");
}

TEST_CASE("Thread PUT adjustment 8.5: invalid, field leader_weight_adjustment", "[device_api]")
{
    expect_bad("{\"leader_weight_adjustment\":8.5}", "leader_weight_adjustment");
}

TEST_CASE("Thread PUT adjustment \"8\": invalid, field leader_weight_adjustment", "[device_api]")
{
    expect_bad("{\"leader_weight_adjustment\":\"8\"}", "leader_weight_adjustment");
}

TEST_CASE("Thread PUT adjustment null: invalid, field leader_weight_adjustment", "[device_api]")
{
    expect_bad("{\"leader_weight_adjustment\":null}", "leader_weight_adjustment");
}

TEST_CASE("Thread PUT adjustment true: invalid, field leader_weight_adjustment", "[device_api]")
{
    expect_bad("{\"leader_weight_adjustment\":true}", "leader_weight_adjustment");
}

TEST_CASE("Thread PUT adjustment 17: invalid, field leader_weight_adjustment", "[device_api]")
{
    expect_bad("{\"leader_weight_adjustment\":17}", "leader_weight_adjustment");
}

TEST_CASE("Thread PUT adjustment 1e30: invalid, field leader_weight_adjustment", "[device_api]")
{
    expect_bad("{\"leader_weight_adjustment\":1e30}", "leader_weight_adjustment");
}

TEST_CASE("Thread PUT adjustment -17: invalid, field leader_weight_adjustment", "[device_api]")
{
    expect_bad("{\"leader_weight_adjustment\":-17}", "leader_weight_adjustment");
}

TEST_CASE("Thread PUT become_leader 1: invalid, field become_leader", "[device_api]")
{
    expect_bad("{\"become_leader\":1}", "become_leader");
}

TEST_CASE("Thread PUT become_leader \"true\": invalid, field become_leader", "[device_api]")
{
    expect_bad("{\"become_leader\":\"true\"}", "become_leader");
}

TEST_CASE("Thread PUT become_leader null: invalid, field become_leader", "[device_api]")
{
    expect_bad("{\"become_leader\":null}", "become_leader");
}

TEST_CASE("Thread PUT bad adjustment next to a good become_leader: invalid, field leader_weight_adjustment", "[device_api]")
{
    expect_bad("{\"become_leader\":true,\"leader_weight_adjustment\":99}", "leader_weight_adjustment");
}

/* ---- device_api_thread_json ---- */

TEST_CASE("Thread JSON weight known (adjustment 8, weight 73): the three fields", "[device_api]")
{
    char buf[256];
    const device_api_thread_t t = {.adjustment = 8, .weight_known = true, .weight = 73};

    TEST_ASSERT_GREATER_THAN(0, render(buf, sizeof(buf), &t));
    TEST_ASSERT_EQUAL_STRING("{\"leader_weight_adjustment\":8,\"leader_weight\":73,\"leader_weight_base\":65}", buf);
}

TEST_CASE("Thread JSON negative adjustment: base is weight minus adjustment", "[device_api]")
{
    char buf[256];
    const device_api_thread_t t = {.adjustment = -5, .weight_known = true, .weight = 60};

    TEST_ASSERT_GREATER_THAN(0, render(buf, sizeof(buf), &t));
    TEST_ASSERT_EQUAL_STRING("{\"leader_weight_adjustment\":-5,\"leader_weight\":60,\"leader_weight_base\":65}", buf);
}

TEST_CASE("Thread JSON weight unknown, not partial: only leader_weight_adjustment", "[device_api]")
{
    char buf[256];
    const device_api_thread_t t = {.adjustment = 3};

    TEST_ASSERT_GREATER_THAN(0, render(buf, sizeof(buf), &t));
    TEST_ASSERT_EQUAL_STRING("{\"leader_weight_adjustment\":3}", buf);
}

TEST_CASE("Thread JSON partial: leader_weight_adjustment and partial true", "[device_api]")
{
    char buf[256];
    const device_api_thread_t t = {.adjustment = 3, .partial = true};

    TEST_ASSERT_GREATER_THAN(0, render(buf, sizeof(buf), &t));
    TEST_ASSERT_EQUAL_STRING("{\"leader_weight_adjustment\":3,\"partial\":true}", buf);
}

TEST_CASE("Thread JSON applied false with detail: applied and detail present", "[device_api]")
{
    char buf[256];
    const device_api_thread_t t = {.adjustment = 3, .has_applied = true, .applied = false, .detail = "Thread is not running"};

    TEST_ASSERT_GREATER_THAN(0, render(buf, sizeof(buf), &t));
    cJSON *json = cJSON_Parse(buf);

    TEST_ASSERT_NOT_NULL(json);
    TEST_ASSERT_TRUE(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(json, "applied")));
    TEST_ASSERT_EQUAL_STRING("Thread is not running", cJSON_GetObjectItemCaseSensitive(json, "detail")->valuestring);
    cJSON_Delete(json);
}

TEST_CASE("Thread JSON applied true without detail: no detail key", "[device_api]")
{
    char buf[256];
    const device_api_thread_t t = {.adjustment = 3, .has_applied = true, .applied = true};

    TEST_ASSERT_GREATER_THAN(0, render(buf, sizeof(buf), &t));
    cJSON *json = cJSON_Parse(buf);

    TEST_ASSERT_NOT_NULL(json);
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json, "applied")));
    TEST_ASSERT_NULL(cJSON_GetObjectItemCaseSensitive(json, "detail"));
    cJSON_Delete(json);
}

TEST_CASE("Thread JSON no applied: no applied key", "[device_api]")
{
    char buf[256];
    const device_api_thread_t t = {.adjustment = 3, .weight_known = true, .weight = 68};

    TEST_ASSERT_GREATER_THAN(0, render(buf, sizeof(buf), &t));
    TEST_ASSERT_NULL(strstr(buf, "applied"));
}

TEST_CASE("Thread JSON become_started: become_leader started", "[device_api]")
{
    char buf[256];
    const device_api_thread_t t = {.adjustment = 0, .become_started = true};

    TEST_ASSERT_GREATER_THAN(0, render(buf, sizeof(buf), &t));
    cJSON *json = cJSON_Parse(buf);

    TEST_ASSERT_NOT_NULL(json);
    TEST_ASSERT_EQUAL_STRING("started", cJSON_GetObjectItemCaseSensitive(json, "become_leader")->valuestring);
    cJSON_Delete(json);
}

TEST_CASE("Thread JSON buffer too small: negative", "[device_api]")
{
    char buf[16];
    const device_api_thread_t t = {.adjustment = 8, .weight_known = true, .weight = 73};

    TEST_ASSERT_LESS_THAN(0, render(buf, sizeof(buf), &t));
}

/* ---- device_api_thread_error_json ---- */

TEST_CASE("Thread error JSON without a stored adjustment: only error and detail", "[device_api]")
{
    char buf[256];

    TEST_ASSERT_GREATER_THAN(0, device_api_thread_error_json(buf, sizeof(buf), "busy", "mode SNIFFER", NULL));
    TEST_ASSERT_EQUAL_STRING("{\"error\":\"busy\",\"detail\":\"mode SNIFFER\"}", buf);
}

TEST_CASE("Thread error JSON with t but no has_applied: only error and detail", "[device_api]")
{
    char buf[256];
    const device_api_thread_t t = {.adjustment = 3};

    TEST_ASSERT_GREATER_THAN(0, device_api_thread_error_json(buf, sizeof(buf), "cannot_take_over", "weight not higher", &t));
    TEST_ASSERT_NULL(strstr(buf, "applied"));
    TEST_ASSERT_NULL(strstr(buf, "leader_weight_adjustment"));
}

TEST_CASE("Thread error JSON after a stored adjustment: adds adjustment and applied, no applied detail", "[device_api]")
{
    char buf[256];
    const device_api_thread_t t = {.adjustment = -4, .has_applied = true, .applied = true, .detail = "ignored"};

    TEST_ASSERT_GREATER_THAN(0, device_api_thread_error_json(buf, sizeof(buf), "cannot_take_over", "weight not higher", &t));
    cJSON *json = cJSON_Parse(buf);

    TEST_ASSERT_NOT_NULL(json);
    TEST_ASSERT_EQUAL_STRING("cannot_take_over", cJSON_GetObjectItemCaseSensitive(json, "error")->valuestring);
    TEST_ASSERT_EQUAL_STRING("weight not higher", cJSON_GetObjectItemCaseSensitive(json, "detail")->valuestring);
    TEST_ASSERT_EQUAL_INT(-4, cJSON_GetObjectItemCaseSensitive(json, "leader_weight_adjustment")->valueint);
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json, "applied")));
    TEST_ASSERT_NULL(strstr(buf, "ignored"));
    cJSON_Delete(json);
}

TEST_CASE("Thread error JSON with applied false: applied is false", "[device_api]")
{
    char buf[256];
    const device_api_thread_t t = {.adjustment = 8, .has_applied = true, .applied = false};

    TEST_ASSERT_GREATER_THAN(0, device_api_thread_error_json(buf, sizeof(buf), "busy", "OpenThread is busy, retry", &t));
    cJSON *json = cJSON_Parse(buf);

    TEST_ASSERT_NOT_NULL(json);
    TEST_ASSERT_TRUE(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(json, "applied")));
    cJSON_Delete(json);
}

TEST_CASE("Thread error JSON buffer too small: negative", "[device_api]")
{
    char buf[16];

    TEST_ASSERT_LESS_THAN(0, device_api_thread_error_json(buf, sizeof(buf), "busy", "OpenThread is busy, retry", NULL));
}

/* ---- device_api_thread_refusal ---- */

static void refusal(br_core_leader_refusal_t r, int mode, int want_status, const char *want_code, const char *want_detail)
{
    int status = 0;
    const char *code = NULL;
    char detail[64] = "";

    device_api_thread_refusal(r, mode, &status, &code, detail, sizeof(detail));
    TEST_ASSERT_EQUAL_INT(want_status, status);
    TEST_ASSERT_EQUAL_STRING(want_code, code);
    TEST_ASSERT_EQUAL_STRING(want_detail, detail);
}

TEST_CASE("Thread refusal WEIGHT_NOT_HIGHER: 409 cannot_take_over, weight not higher", "[device_api]")
{
    refusal(BR_CORE_LEADER_WEIGHT_NOT_HIGHER, BR_MODE_BR_RUNNING, 409, "cannot_take_over", "weight not higher");
}

TEST_CASE("Thread refusal ALREADY: 409 cannot_take_over, already leader", "[device_api]")
{
    refusal(BR_CORE_LEADER_ALREADY, BR_MODE_BR_RUNNING, 409, "cannot_take_over", "already leader");
}

TEST_CASE("Thread refusal NOT_ATTACHED: 409 cannot_take_over, not attached", "[device_api]")
{
    refusal(BR_CORE_LEADER_NOT_ATTACHED, BR_MODE_BR_RUNNING, 409, "cannot_take_over", "not attached");
}

TEST_CASE("Thread refusal NOT_ELIGIBLE: 409 cannot_take_over, not router eligible", "[device_api]")
{
    refusal(BR_CORE_LEADER_NOT_ELIGIBLE, BR_MODE_BR_RUNNING, 409, "cannot_take_over", "not router eligible");
}

TEST_CASE("Thread refusal NOT_RUNNING in RADIO_FAULT: 409 busy, mode RADIO_FAULT", "[device_api]")
{
    refusal(BR_CORE_LEADER_NOT_RUNNING, BR_MODE_RADIO_FAULT, 409, "busy", "mode RADIO_FAULT");
}
