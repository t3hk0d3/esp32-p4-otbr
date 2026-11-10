#include <stdlib.h>
#include <string.h>
#include "unity.h"
#include "unity_test_runner.h"
#include "logging_text.h"

static void expect_target(const char *in, const char *host, uint16_t port)
{
    char h[64];
    uint16_t p = 0;

    TEST_ASSERT_EQUAL_MESSAGE(ESP_OK, logging_target_parse(in, h, sizeof(h), &p), in);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(host, h, in);
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(port, p, in);
}

/* ---- logging_target_parse ---- */

TEST_CASE("valid targets: host and port, 514 when omitted", "[logging]")
{
    expect_target("192.168.1.10", "192.168.1.10", 514);
    expect_target("192.168.1.10:1514", "192.168.1.10", 1514);
    expect_target("[fd00::1]", "fd00::1", 514);
    expect_target("[fd00::1]:1514", "fd00::1", 1514);
    expect_target("logs.lan:514", "logs.lan", 514);
    expect_target("logs.lan:65535", "logs.lan", 65535);
}

TEST_CASE("invalid targets: ESP_ERR_INVALID_ARG, outputs untouched", "[logging]")
{
    static const char *const bad[] = {"", ":514", "host:0", "host:65536", "host:x", "host:", "[fd00::1", "fd00::1:514",
                                      "[]", "[fd00::1]x", "[fd00::1]:", "ho st",
                                      "host:5 14"
                                     };

    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        char h[64] = "keep";
        uint16_t p = 7;

        TEST_ASSERT_EQUAL_MESSAGE(ESP_ERR_INVALID_ARG, logging_target_parse(bad[i], h, sizeof(h), &p), bad[i]);
        TEST_ASSERT_EQUAL_STRING("keep", h);
        TEST_ASSERT_EQUAL_UINT16(7, p);
    }
    char h[4];
    uint16_t p;

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, logging_target_parse(NULL, h, sizeof(h), &p));
}

TEST_CASE("host longer than the buffer: ESP_ERR_INVALID_ARG", "[logging]")
{
    char h[8];
    uint16_t p;

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, logging_target_parse("longhostname.lan", h, sizeof(h), &p));
    TEST_ASSERT_EQUAL(ESP_OK, logging_target_parse("1234567", h, sizeof(h), &p));
}

void app_main(void)
{
    UNITY_BEGIN();
    unity_run_all_tests();
    int failures = (int)UNITY_END();
    exit(failures == 0 ? 0 : 1);
}
