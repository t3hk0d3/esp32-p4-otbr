if(IDF_TARGET STREQUAL "linux")
    # Host tests cover the pure core, the text helpers, the transit ring (FreeRTOS POSIX port spinlock) and the
    # settings store and apply (NVS and esp_log_level_set through the ops seam in logging_priv.h).
    idf_component_register(SRCS "logging_core.c" "logging_text.c" "logging_ring.c" "logging_settings.c"
                           INCLUDE_DIRS "include" "."
                           REQUIRES freertos
                           PRIV_REQUIRES log)
    return()
endif()

idf_component_register(SRCS "logging_core.c" "logging_text.c" "logging_ring.c" "logging_settings.c"
                       INCLUDE_DIRS "include" "."
                       REQUIRES freertos
                       PRIV_REQUIRES log nvs_flash)
