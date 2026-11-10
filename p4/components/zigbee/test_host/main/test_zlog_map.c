#include "esp_log.h"
#include "unity.h"
#include "zigbee_zlog_map.h"

TEST_CASE("each ESP level: maps to ziggurat's 0..5", "[zigbee]")
{
    TEST_ASSERT_EQUAL_UINT8(0, zigbee_zlog_from_esp(ESP_LOG_NONE));
    TEST_ASSERT_EQUAL_UINT8(1, zigbee_zlog_from_esp(ESP_LOG_ERROR));
    TEST_ASSERT_EQUAL_UINT8(2, zigbee_zlog_from_esp(ESP_LOG_WARN));
    TEST_ASSERT_EQUAL_UINT8(3, zigbee_zlog_from_esp(ESP_LOG_INFO));
    TEST_ASSERT_EQUAL_UINT8(4, zigbee_zlog_from_esp(ESP_LOG_DEBUG));
    TEST_ASSERT_EQUAL_UINT8(5, zigbee_zlog_from_esp(ESP_LOG_VERBOSE));
}

TEST_CASE("an ESP level above verbose: maps to 5", "[zigbee]")
{
    TEST_ASSERT_EQUAL_UINT8(5, zigbee_zlog_from_esp((esp_log_level_t)9));
}

TEST_CASE("each ziggurat level 1..5: maps to the ESP level", "[zigbee]")
{
    TEST_ASSERT_EQUAL_INT(ESP_LOG_ERROR, zigbee_zlog_to_esp(1));
    TEST_ASSERT_EQUAL_INT(ESP_LOG_WARN, zigbee_zlog_to_esp(2));
    TEST_ASSERT_EQUAL_INT(ESP_LOG_INFO, zigbee_zlog_to_esp(3));
    TEST_ASSERT_EQUAL_INT(ESP_LOG_DEBUG, zigbee_zlog_to_esp(4));
    TEST_ASSERT_EQUAL_INT(ESP_LOG_VERBOSE, zigbee_zlog_to_esp(5));
}

TEST_CASE("ziggurat level 0 or above 5: clamps to error and verbose", "[zigbee]")
{
    TEST_ASSERT_EQUAL_INT(ESP_LOG_ERROR, zigbee_zlog_to_esp(0));
    TEST_ASSERT_EQUAL_INT(ESP_LOG_VERBOSE, zigbee_zlog_to_esp(200));
}
