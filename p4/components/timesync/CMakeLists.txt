if(IDF_TARGET STREQUAL "linux")
    # Host tests cover the pure rules.
    idf_component_register(SRCS "rules.c"
                           INCLUDE_DIRS "include" ".")
    return()
endif()

idf_component_register(SRCS "rules.c" "timesync.c" "dhcp_wrap.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "."
                       PRIV_REQUIRES board_cores esp_event esp_netif esp_timer freertos log lwip netcfg nvs_flash)
# dhcp_wrap.c sits in front of lwIP's dhcp_set_ntp_servers (the DHCP lease's NTP servers, with their interface). -u
# keeps the wrapper in the link: nothing else refers to it by its own name.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=dhcp_set_ntp_servers" "-Wl,-u,__wrap_dhcp_set_ntp_servers")
