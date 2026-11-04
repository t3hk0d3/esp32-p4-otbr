if(IDF_TARGET STREQUAL "linux")
    # Host tests cover the pure core.
    idf_component_register(SRCS "core.c"
                           INCLUDE_DIRS "include")
    return()
endif()

idf_component_register(SRCS "core.c" "ot_console.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES br_core console esp_timer netnames nvs_flash openthread log)
