if(IDF_TARGET STREQUAL "linux")
    # Host tests cover the pure rules.
    idf_component_register(SRCS "rules.c"
                           INCLUDE_DIRS "include" "."
                           REQUIRES netcfg)
    return()
endif()

idf_component_register(SRCS "rules.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "."
                       REQUIRES netcfg)
