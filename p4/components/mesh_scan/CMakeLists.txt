if(IDF_TARGET STREQUAL "linux")
    # Host tests cover the renderer and the state machine (driven through fake ops).
    idf_component_register(SRCS "render.c" "scan.c"
                           INCLUDE_DIRS "include" "."
                           PRIV_REQUIRES freertos log)
    return()
endif()

# The OpenThread glue (ot_ops.c) joins the sources in the next task.
idf_component_register(SRCS "render.c" "scan.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "."
                       PRIV_REQUIRES freertos log)
