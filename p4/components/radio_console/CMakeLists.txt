if(IDF_TARGET STREQUAL "linux")
    # Host tests cover the pure protocol file only.
    idf_component_register(SRCS "radio_console_proto.c"
                           INCLUDE_DIRS "include" ".")
    return()
endif()

idf_component_register(SRCS "radio_console.c" "radio_console_proto.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "."
                       PRIV_REQUIRES board_cores builtin_rcp console device_api esp_driver_uart esp_timer freertos log radio_mux rcp_sync)
