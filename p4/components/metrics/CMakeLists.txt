if(IDF_TARGET STREQUAL "linux")
    # Host tests cover the pure files only (writer and renderer).
    idf_component_register(SRCS "writer.c" "render.c"
                           INCLUDE_DIRS "include" "."
                           REQUIRES diag)
    return()
endif()

idf_component_register(SRCS "writer.c" "render.c" "collector.c" "metrics_handler.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "."
                       REQUIRES esp_http_server
                       PRIV_REQUIRES board_cores br_core diag netcfg sniffer esp_netif esp_timer esp_partition app_update
                                     esp_app_format freertos openthread log)
