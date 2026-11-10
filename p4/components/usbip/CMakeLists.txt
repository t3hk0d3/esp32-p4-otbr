set(pure_srcs "proto.c" "core.c" "budget.c" "engine.c")

if(IDF_TARGET STREQUAL "linux")
    # Host tests cover the pure sources.
    idf_component_register(SRCS ${pure_srcs}
                           INCLUDE_DIRS "include")
    return()
endif()

# Target-only sources: esp-usb, sockets, NVS.
idf_component_register(SRCS ${pure_srcs} "usb_side.c" "net.c" "usbip.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES board_cores esp_timer freertos heap log espressif__usb lwip netcfg nvs_flash radio_mux vfs)
