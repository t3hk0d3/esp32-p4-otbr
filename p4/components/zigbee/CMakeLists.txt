# Zigbee mode. For now only ziggurat's prebuilt staticlib and its header; the C code that implements the library's
# imports comes with later tasks. No component requires it yet, so MINIMAL_BUILD leaves it out of the image.
if(IDF_TARGET STREQUAL "linux")
    # Host tests build the pure files (CCM*, heap accounting, the WebSocket link rules) only; the cargo step needs the
    # riscv toolchain and stays off the host.
    idf_component_register(SRCS "ccm.c" "heap_stats.c" "link.c"
                           INCLUDE_DIRS "include")
    return()
endif()

idf_component_register()
include(${CMAKE_CURRENT_LIST_DIR}/build_ziggurat.cmake)
