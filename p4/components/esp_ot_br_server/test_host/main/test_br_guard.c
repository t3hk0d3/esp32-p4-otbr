#include <stdlib.h>
#include "unity.h"
#include "unity_test_runner.h"
#include "esp_br_guard.h"

static const br_core_mode_t ALL_MODES[] = {BR_MODE_STARTING, BR_MODE_BR_RUNNING, BR_MODE_SNIFFER, BR_MODE_RADIO_FAULT};
#define MODE_COUNT (sizeof(ALL_MODES) / sizeof(ALL_MODES[0]))

TEST_CASE("Thread-changing endpoint on SNIFFER: returns 409", "[br_guard]")
{
    TEST_ASSERT_EQUAL(409, esp_br_guard_status(ESP_BR_GUARD_THREAD_OP, BR_MODE_SNIFFER, true));
}

TEST_CASE("Thread-changing endpoint on SNIFFER with OpenThread switched off: still returns 409", "[br_guard]")
{
    TEST_ASSERT_EQUAL(409, esp_br_guard_status(ESP_BR_GUARD_THREAD_OP, BR_MODE_SNIFFER, false));
}

TEST_CASE("Thread-changing endpoint on RADIO_FAULT: returns 503", "[br_guard]")
{
    TEST_ASSERT_EQUAL(503, esp_br_guard_status(ESP_BR_GUARD_THREAD_OP, BR_MODE_RADIO_FAULT, false));
    TEST_ASSERT_EQUAL(503, esp_br_guard_status(ESP_BR_GUARD_THREAD_OP, BR_MODE_RADIO_FAULT, true));
}

TEST_CASE("Thread-changing endpoint on STARTING: returns 503", "[br_guard]")
{
    TEST_ASSERT_EQUAL(503, esp_br_guard_status(ESP_BR_GUARD_THREAD_OP, BR_MODE_STARTING, true));
}

TEST_CASE("Thread-changing endpoint on BR_RUNNING: is allowed", "[br_guard]")
{
    TEST_ASSERT_EQUAL(0, esp_br_guard_status(ESP_BR_GUARD_THREAD_OP, BR_MODE_BR_RUNNING, true));
}

TEST_CASE("Thread-changing endpoint on BR_RUNNING with OpenThread switched off: returns 503", "[br_guard]")
{
    TEST_ASSERT_EQUAL(503, esp_br_guard_status(ESP_BR_GUARD_THREAD_OP, BR_MODE_BR_RUNNING, false));
}

TEST_CASE("Read endpoint with OpenThread running: is allowed in every mode", "[br_guard]")
{
    for (size_t i = 0; i < MODE_COUNT; i++) {
        TEST_ASSERT_EQUAL(0, esp_br_guard_status(ESP_BR_GUARD_READ, ALL_MODES[i], true));
    }
}

TEST_CASE("Read endpoint with OpenThread not running: returns 503 in every mode", "[br_guard]")
{
    for (size_t i = 0; i < MODE_COUNT; i++) {
        TEST_ASSERT_EQUAL(503, esp_br_guard_status(ESP_BR_GUARD_READ, ALL_MODES[i], false));
    }
}

TEST_CASE("Endpoint without OpenThread access: is allowed in every mode", "[br_guard]")
{
    for (size_t i = 0; i < MODE_COUNT; i++) {
        TEST_ASSERT_EQUAL(0, esp_br_guard_status(ESP_BR_GUARD_NONE, ALL_MODES[i], false));
        TEST_ASSERT_EQUAL(0, esp_br_guard_status(ESP_BR_GUARD_NONE, ALL_MODES[i], true));
    }
}

TEST_CASE("Unknown endpoint class: is refused with 503", "[br_guard]")
{
    TEST_ASSERT_EQUAL(503, esp_br_guard_status((esp_br_guard_class_t)42, BR_MODE_BR_RUNNING, true));
}

TEST_CASE("Refusal detail for 409 and 503: names the reason", "[br_guard]")
{
    TEST_ASSERT_EQUAL_STRING("sniffer active", esp_br_guard_detail(409));
    TEST_ASSERT_EQUAL_STRING("radio not available", esp_br_guard_detail(503));
}

TEST_CASE("Refusal detail for any other status: is NULL", "[br_guard]")
{
    TEST_ASSERT_NULL(esp_br_guard_detail(0));
    TEST_ASSERT_NULL(esp_br_guard_detail(200));
}

void app_main(void)
{
    UNITY_BEGIN();
    unity_run_all_tests();
    int failures = (int)UNITY_END();
    exit(failures == 0 ? 0 : 1);
}
