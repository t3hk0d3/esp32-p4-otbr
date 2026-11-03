if(IDF_TARGET STREQUAL "linux")
    # Host tests cover the pure files only.
    idf_component_register(SRCS "settings.c" "announce.c" "bgp_msg.c" "bgp_fsm.c"
                           INCLUDE_DIRS "include" ".")
    return()
endif()

# routing_types.h and settings.h sit next to the sources and are part of the public interface (routing.h includes
# them), so "." is a public include directory.
idf_component_register(SRCS "settings.c" "announce.c" "bgp_msg.c" "bgp_fsm.c"
                       INCLUDE_DIRS "include" "."
                       PRIV_REQUIRES nvs_flash log)
