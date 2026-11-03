if(IDF_TARGET STREQUAL "linux")
    # Host tests cover the listener table and the dispatch (mux.c) through fake ops. The openthread component is not
    # built for linux; its public headers are plain C and give the types.
    idf_build_get_property(idf_path IDF_PATH)
    idf_component_register(SRCS "mux.c"
                           INCLUDE_DIRS "include" "." "${idf_path}/components/openthread/openthread/include"
                           PRIV_REQUIRES log)
    return()
endif()

idf_component_register(SRCS "mux.c" "ot_ops.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "."
                       REQUIRES openthread
                       PRIV_REQUIRES log)
