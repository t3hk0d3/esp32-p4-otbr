if(IDF_TARGET STREQUAL "linux")
    # Host tests cover the pure core only: the HTTP client config needs esp_http_client.
    idf_component_register(SRCS "self_update_core.c"
                           INCLUDE_DIRS "include" ".")
    return()
endif()

idf_component_register(SRCS "self_update_core.c" "self_update_http.c"
                       INCLUDE_DIRS "include" "."
                       REQUIRES esp_http_client
                       PRIV_REQUIRES mbedtls esp_common)
