cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../common/rcp_sync_proto"
    "${CMAKE_CURRENT_LIST_DIR}/../common/c6_board")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../tools/base_version.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../tools/test_build.cmake)
project(c6_updater)
