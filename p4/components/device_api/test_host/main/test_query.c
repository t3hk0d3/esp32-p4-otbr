#include <string.h>
#include "unity.h"
#include "unity_test_runner.h"
#include "device_api_priv.h"

TEST_CASE("On a percent-encoded value: it is decoded", "[device_api]")
{
    char out[64];

    TEST_ASSERT_EQUAL(ESP_OK, device_api_query_value("from_url=http%3A%2F%2F10.0.0.2%3A8000%2Fc6%20rcp.bin%3Fa%3D1%26b%3D2", "from_url", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("http://10.0.0.2:8000/c6 rcp.bin?a=1&b=2", out);
}

TEST_CASE("On several parameters: the one with the exact key is found", "[device_api]")
{
    char out[16];

    TEST_ASSERT_EQUAL(ESP_OK, device_api_query_value("xchannel=1&channel=15&duration=9", "channel", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("15", out);
}

TEST_CASE("On a plus sign: it stays a plus sign", "[device_api]")
{
    char out[16];

    TEST_ASSERT_EQUAL(ESP_OK, device_api_query_value("u=a+b", "u", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("a+b", out);
}

TEST_CASE("On a missing key or a NULL query: returns ESP_ERR_NOT_FOUND", "[device_api]")
{
    char out[16];

    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, device_api_query_value("a=1", "b", out, sizeof(out)));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, device_api_query_value(NULL, "b", out, sizeof(out)));
}

TEST_CASE("On a key without a value: the value is empty", "[device_api]")
{
    char out[16] = "x";

    TEST_ASSERT_EQUAL(ESP_OK, device_api_query_value("from_url=&b=1", "from_url", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("", out);
    TEST_ASSERT_EQUAL(ESP_OK, device_api_query_value("from_url", "from_url", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("", out);
}

TEST_CASE("On a value longer than the buffer: returns ESP_ERR_INVALID_SIZE", "[device_api]")
{
    char out[4];

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, device_api_query_value("a=12345", "a", out, sizeof(out)));
}

TEST_CASE("On a bad percent escape: returns ESP_ERR_INVALID_ARG", "[device_api]")
{
    char out[16];

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, device_api_query_value("a=%zz", "a", out, sizeof(out)));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, device_api_query_value("a=%4", "a", out, sizeof(out)));
}

TEST_CASE("On a number inside the range: it is parsed", "[device_api]")
{
    uint32_t v = 0;

    TEST_ASSERT_EQUAL(ESP_OK, device_api_query_uint("channel=26&duration=3600", "duration", 1, 3600, &v));
    TEST_ASSERT_EQUAL(3600, v);
}

TEST_CASE("On a number outside the range or not a number: returns ESP_ERR_INVALID_ARG", "[device_api]")
{
    uint32_t v = 0;

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, device_api_query_uint("channel=10", "channel", 11, 26, &v));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, device_api_query_uint("channel=27", "channel", 11, 26, &v));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, device_api_query_uint("channel=1x", "channel", 11, 26, &v));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, device_api_query_uint("channel=", "channel", 11, 26, &v));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, device_api_query_uint("channel=-1", "channel", 11, 26, &v));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, device_api_query_uint("channel=99999999999", "channel", 11, 26, &v));
}

TEST_CASE("On a number key that is absent: returns ESP_ERR_NOT_FOUND", "[device_api]")
{
    uint32_t v = 0;

    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, device_api_query_uint("duration=5", "channel", 11, 26, &v));
}
