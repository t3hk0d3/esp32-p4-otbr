idf_component_register(SRCS "migrator_main.c" "migrator_core.c"
                       INCLUDE_DIRS "."
                       REQUIRES c6_bundle c6_board
                       PRIV_REQUIRES app_update bootloader_support esp_partition spi_flash)
