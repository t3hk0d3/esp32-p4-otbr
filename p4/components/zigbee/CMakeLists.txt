# Zigbee mode. For now only ziggurat's prebuilt staticlib and its header; the C code that implements the library's
# imports comes with later tasks. No component requires it yet, so MINIMAL_BUILD leaves it out of the image.
if(IDF_TARGET STREQUAL "linux")
    # Host tests build the pure files (CCM*, heap accounting, the WebSocket link rules, the spinel client) only; the
    # cargo step needs the riscv toolchain and stays off the host. The spinel client encodes and decodes with
    # OpenThread's plain-C spinel.c, compiled by path from the IDF tree.
    set(spinel_dir "${IDF_PATH}/components/openthread/openthread/src/lib/spinel")
    idf_component_register(SRCS "ccm.c" "heap_stats.c" "link.c" "spinel_client.c" "${spinel_dir}/spinel.c"
                           INCLUDE_DIRS "include"
                           PRIV_INCLUDE_DIRS "${spinel_dir}")
    return()
endif()

idf_component_register()
include(${CMAKE_CURRENT_LIST_DIR}/build_ziggurat.cmake)
