# Derived from ESP-IDF v6.1 examples/openthread/ot_rcp (IDF_VERSION v6.1).
# Changes against the example: SPI host link and pins from common/c6_board, custom 4 MB
# partition table, project versioning (PROJECT_VER becomes the app descriptor version).
cmake_minimum_required(VERSION 3.22)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../common/c6_board")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../tools/version.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../tools/test_build.cmake)
# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)
project(c6_rcp)

