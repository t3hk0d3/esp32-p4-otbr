if(IDF_TARGET STREQUAL "linux")
    # Host tests cover the pure core (including the capture's routing decision), the text helpers, the transit ring
    # (FreeRTOS POSIX port spinlock), the settings store and apply (NVS and esp_log_level_set through the ops seam in
    # logging_priv.h), the drain's sinks (USB-JTAG, resolver and UDP through the drain ops seam) and the live viewers.
    # The capture (linker wraps of esp_log) and the drain task are target-only.
    idf_component_register(SRCS "logging_core.c" "logging_text.c" "logging_ring.c" "logging_settings.c"
                                "logging_drain.c" "logging_viewers.c"
                           INCLUDE_DIRS "include" "."
                           REQUIRES freertos log heap)
    return()
endif()

idf_component_register(SRCS "logging_core.c" "logging_text.c" "logging_ring.c" "logging_settings.c"
                            "logging_drain.c" "logging_viewers.c" "logging_capture.c" "logging_console.c"
                       INCLUDE_DIRS "include" "."
                       REQUIRES freertos log
                       PRIV_REQUIRES console nvs_flash esp_driver_usb_serial_jtag lwip netnames esp_timer board_cores)

# Every ESP-IDF log call goes through logging_capture.c: the ESP_LOGx macros and esp_log_buffer_* call esp_log,
# esp_log_write/esp_log_writev call esp_log_va from another translation unit. Linking the component in (main REQUIRES
# it) makes the linker resolve the __wrap_ symbols from it.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_log" "-Wl,--wrap=esp_log_va")
