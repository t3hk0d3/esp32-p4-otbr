idf_component_register(SRCS "installer_main.c" "installer_core.c"
                       INCLUDE_DIRS "."
                       REQUIRES c6_board
                       PRIV_REQUIRES app_update bootloader_support esp_partition spi_flash
                       EMBED_FILES "${CMAKE_BINARY_DIR}/c6_images/c6_bootloader.bin"
                                   "${CMAKE_BINARY_DIR}/c6_images/c6_partition_table.bin"
                                   "${CMAKE_BINARY_DIR}/c6_images/c6_updater.bin")
