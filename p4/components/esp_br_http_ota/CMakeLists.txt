if(IDF_TARGET STREQUAL "linux")
    # Host tests cover the bundle header parser and the chunk state machine only; the flash backend needs the device.
    idf_component_register(SRCS "src/ota_bundle.c" "src/ota_stream.c"
                           INCLUDE_DIRS include)
    return()
endif()

idf_component_register(SRC_DIRS src
                       INCLUDE_DIRS include
                       REQUIRES app_update esp_http_client esp_http_server
                       PRIV_REQUIRES esp_partition mbedtls)
