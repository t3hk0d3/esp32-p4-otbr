# c6_migrator: the C6 app that the P4 sends through the factory ESP-Hosted OTA, followed by an install bundle
# (common/c6_bundle). Booted by the stock bootloader, it writes our RCP, updater, bootloader and partition table, then
# restarts into them. It never runs on a C6 that already has our layout.
cmake_minimum_required(VERSION 3.22)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../common/c6_board" "${CMAKE_CURRENT_LIST_DIR}/../common/c6_bundle")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../tools/version.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../tools/test_build.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(c6_migrator)
