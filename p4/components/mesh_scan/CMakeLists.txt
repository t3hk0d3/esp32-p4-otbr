if(IDF_TARGET STREQUAL "linux")
    # Host tests cover the pure files only (the JSON renderer).
    idf_component_register(SRCS "render.c"
                           INCLUDE_DIRS "include" ".")
    return()
endif()

# The state machine and the OpenThread glue join the sources in the next tasks.
idf_component_register(SRCS "render.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS ".")
