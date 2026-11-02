if(IDF_TARGET STREQUAL "linux")
    # Host tests cover the pure parts: framing, RPC encoding and the session over a scripted link.
    idf_component_register(SRCS "hosted_frame.c" "hosted_rpc.c" "hosted_session.c"
                           INCLUDE_DIRS "include" "."
                           PRIV_REQUIRES log)
else()
    idf_component_register(SRCS "hosted_frame.c" "hosted_rpc.c" "hosted_session.c" "hosted_link_sdio.c"
                           INCLUDE_DIRS "include"
                           PRIV_REQUIRES log esp_driver_gpio esp_driver_sdmmc sdmmc esp_timer freertos
                                         espressif__esp_serial_slave_link)
    # board.h (the C6 lines) lives in the app's main directory.
    target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../main")
endif()
