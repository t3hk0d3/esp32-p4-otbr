if(IDF_TARGET STREQUAL "linux")
    # Host tests cover the mode state machine only; it needs nothing from OpenThread but one type (stubbed).
    idf_component_register(SRCS "mode_fsm.c"
                           INCLUDE_DIRS "include" "." "test_host/stubs"
                           REQUIRES rcp_sync
                           PRIV_REQUIRES log)
    return()
endif()

set(srcs "br_core.c" "mode_fsm.c" "validity.c")
if(OTBR_TEST_BUILD)
    list(APPEND srcs "br_core_test.c")
endif()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "include"
                       REQUIRES esp_netif rcp_sync
                       PRIV_REQUIRES board_cores openthread esp_ot_br_server mdns esp_event esp_timer esp_hw_support console
                                     esp_driver_gpio app_update vfs nvs_flash log)
# board.h lives in main (which depends on this component); include it by path.
target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../main")
