#include <string.h>
#include "unity.h"
#include "unity_test_runner.h"
#include "device_api_priv.h"

#define MAX 0x180000

TEST_CASE("On a length of zero: 400", "[device_api]")
{
    TEST_ASSERT_EQUAL(400, device_api_length_check(0, MAX));
}

TEST_CASE("On a length within the cap, including the cap: accepted", "[device_api]")
{
    TEST_ASSERT_EQUAL(0, device_api_length_check(1, MAX));
    TEST_ASSERT_EQUAL(0, device_api_length_check(MAX, MAX));
}

TEST_CASE("On a length above the cap: 413", "[device_api]")
{
    TEST_ASSERT_EQUAL(413, device_api_length_check(MAX + 1, MAX));
    TEST_ASSERT_EQUAL(413, device_api_length_check((size_t)0x7fffffff, MAX));
}

TEST_CASE("On an http URL: accepted", "[device_api]")
{
    TEST_ASSERT_EQUAL(0, device_api_check_url("http://10.0.0.2:8000/c6_rcp.bin").http_status);
}

TEST_CASE("On an https URL: accepted", "[device_api]")
{
    TEST_ASSERT_EQUAL(0, device_api_check_url("https://example.org/a.bin").http_status);
}

TEST_CASE("On an empty, scheme-less, other-scheme or host-less URL: 400 invalid_request", "[device_api]")
{
    const char *bad[] = {"", "example.org/a", "ftp://x/a", "http://", "https://", NULL};

    for (int i = 0; i < 5; i++) {
        TEST_ASSERT_EQUAL(400, device_api_check_url(bad[i]).http_status);
        TEST_ASSERT_EQUAL_STRING("invalid_request", device_api_check_url(bad[i]).code);
        TEST_ASSERT_NOT_NULL(strstr(device_api_check_url(bad[i]).detail, "http"));
    }
    TEST_ASSERT_EQUAL(400, device_api_check_url(bad[5]).http_status);
}

TEST_CASE("On a known installed version: the reply carries it", "[device_api]")
{
    char buf[64];

    TEST_ASSERT_EQUAL(strlen("{\"installed\":\"1.2.3\"}"), device_api_installed_json(buf, sizeof(buf), true, "1.2.3"));
    TEST_ASSERT_EQUAL_STRING("{\"installed\":\"1.2.3\"}", buf);
}

TEST_CASE("On an unknown or empty installed version: the reply carries null", "[device_api]")
{
    char buf[64];

    device_api_installed_json(buf, sizeof(buf), false, "1.2.3");
    TEST_ASSERT_EQUAL_STRING("{\"installed\":null}", buf);
    device_api_installed_json(buf, sizeof(buf), true, "");
    TEST_ASSERT_EQUAL_STRING("{\"installed\":null}", buf);
}

TEST_CASE("On a reply buffer that is too small: returns -1", "[device_api]")
{
    char buf[8];

    TEST_ASSERT_EQUAL(-1, device_api_installed_json(buf, sizeof(buf), true, "1.2.3"));
}
