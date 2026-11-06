if(IDF_TARGET STREQUAL "linux")
    # Host tests cover the pure core.
    idf_component_register(SRCS "core.c" "detect.c" "hdlc.c" "pool.c"
                           INCLUDE_DIRS "include")
    return()
endif()

idf_component_register(SRCS "core.c" "detect.c" "hdlc.c" "pool.c" "mux.c" "mux_api.c" "mux_deliver.c" "mux_nvs.c" "usb_transport.c"
                       INCLUDE_DIRS "include"
                       REQUIRES freertos
                       PRIV_REQUIRES board_cores esp_ringbuf esp_timer heap log nvs_flash vfs
                                     espressif__usb espressif__usb_host_cdc_acm espressif__usb_host_cp210x_vcp
                                     espressif__usb_host_ch34x_vcp)
