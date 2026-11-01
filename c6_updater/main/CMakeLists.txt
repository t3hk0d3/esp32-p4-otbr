idf_component_register(SRCS "updater_main.c" "updater_core.c"
                       INCLUDE_DIRS "."
                       REQUIRES rcp_sync_proto c6_board
                       PRIV_REQUIRES app_update esp_driver_spi esp_driver_gpio esp_partition bootloader_support)
