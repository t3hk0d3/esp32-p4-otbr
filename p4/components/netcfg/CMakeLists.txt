if(IDF_TARGET STREQUAL "linux")
    # Host tests cover the pure files: configuration, policy, confirm-or-revert state machine, VLAN frame demux.
    idf_component_register(SRCS "config.c" "policy.c" "trial.c" "vlan_frame.c" "isolation.c" "mdns_rules.c"
                           INCLUDE_DIRS "include" "."
                           REQUIRES esp_netif)
    return()
endif()

idf_component_register(SRCS "config.c" "policy.c" "trial.c" "vlan_frame.c" "isolation.c" "mdns_rules.c" "netcfg.c" "ifaces.c" "vlan_glue.c" "ip6_hooks.c" "mdns_filter.c"
                              "console.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "."
                       REQUIRES esp_eth esp_netif esp_http_server
                       PRIV_REQUIRES board_cores mdns nvs_flash esp_timer esp_event esp_system lwip console log)

# --wrap=udp_recv puts mdns_filter.c in front of the receive callback of the mDNS library (own announcements, spec 5.13); -u pulls
# its archive member in, nothing else references it.
# The prebuilt libopenthread_br.a defines lwip_hook_ip6_input and lwip_hook_ip6_route; ip6_hooks.c chains them (spec 5.13).
# INTERFACE link options reach the final link; idf_build_set_property(LINK_OPTIONS) after project() would not.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=lwip_hook_ip6_input" "-Wl,--wrap=lwip_hook_ip6_route" "-Wl,--wrap=udp_recv" "-Wl,-u,__wrap_udp_recv")
