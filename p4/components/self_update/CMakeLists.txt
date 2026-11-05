if(IDF_TARGET STREQUAL "linux")
    # Nothing builds on the host yet: the HTTP client config needs esp_http_client.
    idf_component_register()
    return()
endif()

idf_component_register(SRCS "self_update_http.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_http_client
                       PRIV_REQUIRES mbedtls esp_common)
