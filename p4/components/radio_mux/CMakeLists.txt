if(IDF_TARGET STREQUAL "linux")
    # Host tests cover the pure core.
    idf_component_register(SRCS "core.c" "detect.c" "hdlc.c" "orch_core.c" "pool.c"
                           INCLUDE_DIRS "include")
    return()
endif()

idf_component_register(SRCS "core.c" "detect.c" "hdlc.c" "orch_core.c" "pool.c" "mux.c" "mux_api.c" "mux_deliver.c"
                            "mux_nvs.c" "usb_transport.c" "c6_transport.c"
                       INCLUDE_DIRS "include"
                       REQUIRES freertos
                       PRIV_REQUIRES board_cores esp_driver_gpio esp_driver_spi esp_ringbuf esp_rom esp_timer heap log
                                     nvs_flash rcp_sync vfs
                                     espressif__usb usb_host_cdc_acm espressif__usb_host_cp210x_vcp
                                     espressif__usb_host_ch34x_vcp)
# The C6 pins (board.h) live in main; included by path, as rcp_sync does.
target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../main")
