#include <stdlib.h>
#include <string.h>
#include "unity.h"
#include "unity_test_runner.h"
#include "device_api_priv.h"

TEST_CASE("On a string with quotes, backslashes and control characters: they are escaped", "[device_api]")
{
    char buf[64];
    json_out_t w;

    json_out_init(&w, buf, sizeof(buf));
    json_out_str(&w, "a\"b\\c\n\x01");
    TEST_ASSERT_FALSE(w.overflow);
    TEST_ASSERT_EQUAL_STRING("\"a\\\"b\\\\c\\n\\u0001\"", buf);
}

TEST_CASE("On valid UTF-8: it is written unchanged", "[device_api]")
{
    char buf[32];
    json_out_t w;

    json_out_init(&w, buf, sizeof(buf));
    json_out_str(&w, "\xc3\xa9\xe2\x82\xac");
    TEST_ASSERT_EQUAL_STRING("\"\xc3\xa9\xe2\x82\xac\"", buf);
}

TEST_CASE("On bytes that are not valid UTF-8: each bad byte becomes a question mark", "[device_api]")
{
    char buf[32];
    json_out_t w;

    json_out_init(&w, buf, sizeof(buf));
    json_out_str(&w, "a\xff" "b\xc3" "c\xe2\x82");
    TEST_ASSERT_EQUAL_STRING("\"a?b?c??\"", buf);
}

TEST_CASE("On a NULL string: null is written", "[device_api]")
{
    char buf[16];
    json_out_t w;

    json_out_init(&w, buf, sizeof(buf));
    json_out_str(&w, NULL);
    TEST_ASSERT_EQUAL_STRING("null", buf);
}

TEST_CASE("On numbers and booleans: they are written in JSON form", "[device_api]")
{
    char buf[32];
    json_out_t w;

    json_out_init(&w, buf, sizeof(buf));
    json_out_uint(&w, 4000000000u);
    json_out_raw(&w, ",");
    json_out_int(&w, -7);
    json_out_raw(&w, ",");
    json_out_bool(&w, true);
    TEST_ASSERT_EQUAL_STRING("4000000000,-7,true", buf);
}

TEST_CASE("On output larger than the buffer: overflow is flagged and the buffer stays NUL-terminated", "[device_api]")
{
    char buf[8];
    json_out_t w;

    json_out_init(&w, buf, sizeof(buf));
    json_out_str(&w, "0123456789");
    TEST_ASSERT_TRUE(w.overflow);
    TEST_ASSERT_EQUAL(7, strlen(buf));
}

void app_main(void)
{
    UNITY_BEGIN();
    unity_run_all_tests();
    int failures = (int)UNITY_END();
    exit(failures == 0 ? 0 : 1);
}
