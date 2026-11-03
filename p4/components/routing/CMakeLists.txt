set(pure_srcs "settings.c" "announce.c" "bgp_msg.c" "bgp_fsm.c" "ripng.c" "publisher.c" "engine.c")

if(IDF_TARGET STREQUAL "linux")
    # Host tests cover the pure files only.
    idf_component_register(SRCS ${pure_srcs}
                           INCLUDE_DIRS "include" ".")
    return()
endif()

# routing_types.h and settings.h sit next to the sources and are part of the public interface (routing.h includes
# them), so "." is a public include directory.
# openthread is public: routing.h names otInstance (routing_on_border_router_up).
idf_component_register(SRCS ${pure_srcs} "ot_ops.c" "task.c" "console.c"
                       INCLUDE_DIRS "include" "."
                       REQUIRES openthread
                       PRIV_REQUIRES nvs_flash log lwip esp_netif esp_timer freertos console board_cores br_core
                                     netcfg esp_ot_br_server) # esp_ot_br_server: ESP_BR_WEB_MAX_OPEN_SOCKETS
