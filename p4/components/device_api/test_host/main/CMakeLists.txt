idf_component_register(SRCS "test_json.c" "test_query.c" "test_long_op.c" "test_errors.c" "test_status.c" "test_limits.c"
                       REQUIRES device_api rcp_sync unity
                       WHOLE_ARCHIVE)
target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../../br_core/include"
                           "${CMAKE_CURRENT_LIST_DIR}/../../../esp_br_http_ota/include")
