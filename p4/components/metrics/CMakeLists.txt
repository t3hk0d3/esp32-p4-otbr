if(IDF_TARGET STREQUAL "linux")
    # Host tests cover the pure files only (writer and renderer).
    idf_component_register(SRCS "writer.c" "render.c"
                           INCLUDE_DIRS "include" "."
                           REQUIRES diag)
    return()
endif()

idf_component_register(SRCS "writer.c" "render.c" "collector.c" "scrape.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "."
                       PRIV_REQUIRES board_cores br_core diag esp_timer freertos openthread log)
