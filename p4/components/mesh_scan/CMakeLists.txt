if(IDF_TARGET STREQUAL "linux")
    # Host tests cover the renderer and the state machine (driven through fake ops).
    idf_component_register(SRCS "render.c" "scan.c" "srp.c"
                           INCLUDE_DIRS "include" "."
                           PRIV_REQUIRES freertos log)
    return()
endif()

idf_component_register(SRCS "render.c" "scan.c" "srp.c" "ot_ops.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "."
                       PRIV_REQUIRES openthread netdiag_mux br_core esp_timer freertos log heap)
