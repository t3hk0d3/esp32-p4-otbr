if(IDF_TARGET STREQUAL "linux")
    # Host tests cover the REST guard decision logic only (test_host/); br_core_mode.h is a plain enum header.
    idf_component_register(SRCS "src/esp_br_guard.c"
                           INCLUDE_DIRS "private_include" "${CMAKE_CURRENT_LIST_DIR}/../br_core/include")
    return()
endif()

set(requires mdns fatfs spiffs esp_eth nvs_flash freertos openthread esp_http_server protocol_examples_common esp_wifi esp_app_format esp_timer)
# Patch 2: the REST guard reads the mode from br_core (br_core in turn requires this component for the ready flag).
# Patch 12: the per-interface policy (netcfg) is asked at every dispatch point.
list(APPEND requires br_core netcfg)
# Patch 17: every Network Diagnostic Get goes through netdiag_mux (one OpenThread netdiag callback for every sender).
list(APPEND requires netdiag_mux)

# For IDF < 6.0, use IDF's built-in json component; for IDF >= 6.0 use managed cJSON component through idf_component.yml
if("${IDF_VERSION_MAJOR}" VERSION_LESS "6")
    list(APPEND requires json)
endif()

idf_component_register(
    SRC_DIRS src
    INCLUDE_DIRS include
    PRIV_INCLUDE_DIRS private_include
    REQUIRES esp_http_server
    PRIV_REQUIRES ${requires}
    EMBED_FILES "favicon.ico"
    EMBED_TXTFILES "frontend/wifi_configuration.html"
)

