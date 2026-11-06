if(IDF_TARGET STREQUAL "linux")
    # Host tests cover the pure core.
    idf_component_register(SRCS "core.c"
                           INCLUDE_DIRS "include"
                           REQUIRES radio_mux)
    return()
endif()

idf_component_register(SRCS "core.c" "proxy.c" "adapter_tcp.c" "adapter_udp.c"
                       INCLUDE_DIRS "include"
                       REQUIRES radio_mux
                       PRIV_REQUIRES board_cores netcfg lwip log freertos esp_timer mdns vfs)
