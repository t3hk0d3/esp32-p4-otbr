if(IDF_TARGET STREQUAL "linux")
    # Host tests cover the core and the check state: the HTTP client needs esp_http_client (fake ops there).
    idf_component_register(SRCS "self_update_core.c" "self_update.c"
                           INCLUDE_DIRS "include" ".")
    return()
endif()

idf_component_register(SRCS "self_update_core.c" "self_update.c" "self_update_http.c"
                       INCLUDE_DIRS "include" "."
                       REQUIRES esp_http_client
                       PRIV_REQUIRES mbedtls esp_common esp_timer)
