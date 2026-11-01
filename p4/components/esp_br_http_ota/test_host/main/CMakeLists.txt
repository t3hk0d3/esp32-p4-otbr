idf_component_register(SRCS "test_ota_bundle.c" "test_ota_stream.c"
                       REQUIRES esp_br_http_ota unity
                       WHOLE_ARCHIVE)
