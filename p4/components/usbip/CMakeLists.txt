set(pure_srcs "proto.c")

if(IDF_TARGET STREQUAL "linux")
    # Host tests cover the pure sources.
    idf_component_register(SRCS ${pure_srcs}
                           INCLUDE_DIRS "include")
    return()
endif()

# Target-only sources (usb_side.c, net.c, usbip.c) join here as they are added.
idf_component_register(SRCS ${pure_srcs}
                       INCLUDE_DIRS "include")
