set(srcs "app_main.c" "console.c" "eth.c" "storage.c")
if(OTBR_TEST_BUILD)
    list(APPEND srcs "test_cmds.c")
endif()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "."
                       REQUIRES esp_eth esp_netif esp_event nvs_flash spiffs app_update console espcoredump esp_timer esp_partition
                       PRIV_REQUIRES board_cores builtin_rcp netcfg netnames c6_board esp_ot_br_server esp_br_http_ota rcp_sync br_core sniffer device_api diag metrics mesh_scan routing openthread ot_console radio_mux rcp_proxy reboot_gate supervisor self_update vfs zigbee
                       EMBED_FILES "${CMAKE_BINARY_DIR}/c6_images/c6_bootloader.bin"
                                   "${CMAKE_BINARY_DIR}/c6_images/c6_partition_table.bin"
                                   "${CMAKE_BINARY_DIR}/c6_images/c6_updater.bin"
                                   "${CMAKE_BINARY_DIR}/c6_images/c6_base_version.txt")
