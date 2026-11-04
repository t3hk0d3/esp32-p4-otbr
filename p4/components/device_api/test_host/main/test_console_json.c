#include <string.h>
#include "unity.h"
#include "unity_test_runner.h"
#include "console_json.h"

/* ---- device_api_console_json ---- */

TEST_CASE("Console GET, no session: session is null", "[device_api]")
{
    char buf[128];

    TEST_ASSERT_GREATER_THAN(0, device_api_console_json(buf, sizeof(buf), false, NULL, 0));
    TEST_ASSERT_EQUAL_STRING("{\"enabled\":false,\"session\":null}", buf);
}

TEST_CASE("Console GET, a session: peer and age", "[device_api]")
{
    char buf[128];

    TEST_ASSERT_GREATER_THAN(0, device_api_console_json(buf, sizeof(buf), true, "192.168.10.23", 812));
    TEST_ASSERT_EQUAL_STRING("{\"enabled\":true,\"session\":{\"peer\":\"192.168.10.23\",\"since_s\":812}}", buf);
}

TEST_CASE("Console GET, buffer too small: -1", "[device_api]")
{
    char buf[16];

    TEST_ASSERT_EQUAL(-1, device_api_console_json(buf, sizeof(buf), true, "192.168.10.23", 812));
}

/* ---- device_api_console_parse ---- */

TEST_CASE("Console PUT, enabled true or false: parsed", "[device_api]")
{
    bool on = false;

    TEST_ASSERT_EQUAL(ESP_OK, device_api_console_parse("{\"enabled\":true}", &on));
    TEST_ASSERT_TRUE(on);
    TEST_ASSERT_EQUAL(ESP_OK, device_api_console_parse("{\"enabled\":false}", &on));
    TEST_ASSERT_FALSE(on);
}

TEST_CASE("Console PUT, enabled missing or not a boolean: invalid", "[device_api]")
{
    bool on = false;

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, device_api_console_parse("{\"enabled\":\"yes\"}", &on));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, device_api_console_parse("{}", &on));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, device_api_console_parse("{\"enabled\":", &on));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, device_api_console_parse("[true]", &on));
}
