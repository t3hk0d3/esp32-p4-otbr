# ROM recovery needs the C6 flash layout constants; board.h and c6_board.h live outside this component, include them by path.
set(c6_board_dir "${CMAKE_CURRENT_LIST_DIR}/../../../common/c6_board")

if(IDF_TARGET STREQUAL "linux")
    # Host tests cover the pure logic only (sync, recovery rules, ROM plan, log ring); the SPI link, UART and flasher need the device.
    idf_component_register(SRCS "sync.c" "recovery_policy.c" "rom_plan.c" "log_ring.c" "log_limiter.c" "uart_handoff.c"
                           INCLUDE_DIRS "include" "."
                           REQUIRES rcp_sync_proto
                           PRIV_REQUIRES log freertos)
    target_include_directories(${COMPONENT_LIB} PRIVATE "${c6_board_dir}")
else()
    idf_component_register(SRCS "sync.c" "recovery_policy.c" "rom_plan.c" "log_ring.c" "log_limiter.c" "uart_handoff.c"
                                "link_spi.c" "base_image.c" "jumper.c" "rom_flash.c" "run_device.c"
                           INCLUDE_DIRS "include" "."
                           REQUIRES rcp_sync_proto
                           PRIV_REQUIRES board_cores c6_hosted esp_driver_gpio esp_driver_spi esp_driver_uart esp_timer esp_rom log freertos
                                         espressif__esp-serial-flasher)
    target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../main" "${c6_board_dir}")
endif()
