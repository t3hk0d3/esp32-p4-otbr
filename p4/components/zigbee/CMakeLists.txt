# Zigbee mode. For now only ziggurat's prebuilt staticlib and its header; the C code that implements the library's
# imports comes with later tasks. No component requires it yet, so MINIMAL_BUILD leaves it out of the image.
idf_component_register()
include(${CMAKE_CURRENT_LIST_DIR}/build_ziggurat.cmake)
