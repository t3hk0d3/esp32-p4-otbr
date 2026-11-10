if(IDF_TARGET STREQUAL "linux")
    # Host tests cover the pure core and the text helpers.
    idf_component_register(SRCS "logging_core.c" "logging_text.c"
                           INCLUDE_DIRS "include" "."
                           PRIV_REQUIRES log)
    return()
endif()

idf_component_register(SRCS "logging_core.c" "logging_text.c"
                       INCLUDE_DIRS "include" "."
                       PRIV_REQUIRES log)
