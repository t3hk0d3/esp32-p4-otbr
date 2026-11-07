if(IDF_TARGET STREQUAL "linux")
    # Host tests cover the pure files only (window arithmetic, lock bookkeeping, JSON rendering).
    idf_component_register(SRCS "windows.c" "lock_stats.c" "snapshot_json.c" "tsens_conv.c"
                           INCLUDE_DIRS "include" "."
                           PRIV_REQUIRES device_api)
    return()
endif()

idf_component_register(SRCS "windows.c" "lock_stats.c" "snapshot_json.c" "tsens_conv.c" "sampler.c" "ot_lock_wrap.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "." "${CMAKE_CURRENT_LIST_DIR}/../device_api" # device_api_priv.h (json_out)
                       PRIV_REQUIRES board_cores device_api openthread freertos esp_timer esp_system heap log esp_driver_tsens esp_hal_ana_conv efuse)
# Statistics of the OpenThread lock without patching upstream: every caller of esp_openthread_lock_acquire/release in the
# link (br_core, esp_ot_br_server, esp_openthread itself) goes through ot_lock_wrap.c. INTERFACE so that the final link
# of the app sees the options.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_openthread_lock_acquire"
                      "-Wl,--wrap=esp_openthread_lock_release")
