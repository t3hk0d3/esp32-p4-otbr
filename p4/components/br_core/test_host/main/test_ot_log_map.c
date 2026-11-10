#include "ot_log_map.h"
#include "unity.h"

TEST_CASE("OT line level CRIT: logged at error", "[ot_log]")
{
    TEST_ASSERT_EQUAL(ESP_LOG_ERROR, br_core_ot_log_to_esp(OT_LOG_LEVEL_CRIT));
}

TEST_CASE("OT line level WARN: logged at warning", "[ot_log]")
{
    TEST_ASSERT_EQUAL(ESP_LOG_WARN, br_core_ot_log_to_esp(OT_LOG_LEVEL_WARN));
}

TEST_CASE("OT line level NOTE: logged at info", "[ot_log]")
{
    TEST_ASSERT_EQUAL(ESP_LOG_INFO, br_core_ot_log_to_esp(OT_LOG_LEVEL_NOTE));
}

TEST_CASE("OT line level INFO: logged at info", "[ot_log]")
{
    TEST_ASSERT_EQUAL(ESP_LOG_INFO, br_core_ot_log_to_esp(OT_LOG_LEVEL_INFO));
}

TEST_CASE("OT line level DEBG: logged at debug", "[ot_log]")
{
    TEST_ASSERT_EQUAL(ESP_LOG_DEBUG, br_core_ot_log_to_esp(OT_LOG_LEVEL_DEBG));
}

TEST_CASE("OT line level unknown (NONE, 99): logged at debug", "[ot_log]")
{
    TEST_ASSERT_EQUAL(ESP_LOG_DEBUG, br_core_ot_log_to_esp(OT_LOG_LEVEL_NONE));
    TEST_ASSERT_EQUAL(ESP_LOG_DEBUG, br_core_ot_log_to_esp(99));
}

TEST_CASE("ESP level off: OpenThread NONE", "[ot_log]")
{
    TEST_ASSERT_EQUAL(OT_LOG_LEVEL_NONE, br_core_esp_to_ot_log(ESP_LOG_NONE));
}

TEST_CASE("ESP level error: OpenThread CRIT", "[ot_log]")
{
    TEST_ASSERT_EQUAL(OT_LOG_LEVEL_CRIT, br_core_esp_to_ot_log(ESP_LOG_ERROR));
}

TEST_CASE("ESP level warning: OpenThread WARN", "[ot_log]")
{
    TEST_ASSERT_EQUAL(OT_LOG_LEVEL_WARN, br_core_esp_to_ot_log(ESP_LOG_WARN));
}

TEST_CASE("ESP level info: OpenThread INFO", "[ot_log]")
{
    TEST_ASSERT_EQUAL(OT_LOG_LEVEL_INFO, br_core_esp_to_ot_log(ESP_LOG_INFO));
}

TEST_CASE("ESP level debug and verbose: OpenThread DEBG", "[ot_log]")
{
    TEST_ASSERT_EQUAL(OT_LOG_LEVEL_DEBG, br_core_esp_to_ot_log(ESP_LOG_DEBUG));
    TEST_ASSERT_EQUAL(OT_LOG_LEVEL_DEBG, br_core_esp_to_ot_log(ESP_LOG_VERBOSE));
}
