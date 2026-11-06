if(IDF_TARGET STREQUAL "linux")
    # Host tests cover the pure core.
    idf_component_register(SRCS "core.c"
                           INCLUDE_DIRS "include")
    return()
endif()

idf_component_register(SRCS "core.c" "usb_serial.c" "usb_host_glue.c" "tcp.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES netcfg nvs_flash lwip log freertos esp_timer board_cores
                                     espressif__usb espressif__usb_host_cdc_acm espressif__usb_host_cp210x_vcp
                                     espressif__usb_host_ch34x_vcp)
