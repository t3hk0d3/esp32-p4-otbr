if(IDF_TARGET STREQUAL "linux")
    # Host tests cover the pure decisions.
    idf_component_register(SRCS "builtin_rcp_core.c"
                           INCLUDE_DIRS "include"
                           REQUIRES radio_mux rcp_sync)
    return()
endif()

idf_component_register(SRCS "builtin_rcp_core.c" "builtin_rcp.c" "c6.c"
                       INCLUDE_DIRS "include"
                       REQUIRES radio_mux rcp_sync
                       PRIV_REQUIRES esp_driver_gpio freertos log nvs_flash)
# The C6 pins (board.h) live in main; included by path, as rcp_sync and radio_mux do.
target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../main")
