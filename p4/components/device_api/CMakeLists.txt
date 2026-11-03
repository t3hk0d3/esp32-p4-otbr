if(IDF_TARGET STREQUAL "linux")
    # Host tests cover the pure helpers only (JSON, query parsing, operation arbiter, error mapping).
    idf_component_register(SRCS "json_out.c" "json_err.c" "query.c" "long_op.c" "limits.c" "status_json.c" "names_json.c" "thread_json.c" "topology.c"
                           INCLUDE_DIRS "include" "."
                           PRIV_INCLUDE_DIRS "${CMAKE_CURRENT_LIST_DIR}/../br_core/include" "${CMAKE_CURRENT_LIST_DIR}/../esp_br_http_ota/include"
                           PRIV_REQUIRES log rcp_sync netnames br_core espressif__cjson routing) # routing: routing_types.h (status_json.c)
    return()
endif()

idf_component_register(SRCS "device_api.c" "diag_handler.c" "json_out.c" "json_err.c" "query.c" "long_op.c" "limits.c" "status_json.c" "names_json.c" "thread_json.c" "topology.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "."
                       REQUIRES esp_http_server
                       PRIV_REQUIRES board_cores br_core mesh_scan netcfg netnames rcp_sync routing sniffer diag metrics esp_br_http_ota esp_http_client
                                     esp_netif esp_timer esp_partition c6_board app_update esp_app_format esp_system esp_driver_gpio lwip openthread log)
# board.h (test builds only: the C6 reset pin) lives in main, which depends on this component; include it by path.
target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../main")

