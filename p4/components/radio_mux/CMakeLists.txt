if(IDF_TARGET STREQUAL "linux")
    # Host tests cover the pure core.
    idf_component_register(SRCS "core.c"
                           INCLUDE_DIRS "include")
    return()
endif()

# Board files join this list in later tasks.
idf_component_register(SRCS "core.c"
                       INCLUDE_DIRS "include")
