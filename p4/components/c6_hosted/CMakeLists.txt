if(IDF_TARGET STREQUAL "linux")
    # Host tests cover the pure parts: framing, RPC encoding and the session over a scripted link.
    idf_component_register(SRCS "hosted_frame.c" "hosted_rpc.c" "hosted_session.c"
                           INCLUDE_DIRS "include" "."
                           PRIV_REQUIRES log)
else()
    idf_component_register(SRCS "hosted_frame.c" "hosted_rpc.c" "hosted_session.c"
                           INCLUDE_DIRS "include"
                           PRIV_REQUIRES log)
endif()
