cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/.." "${CMAKE_CURRENT_LIST_DIR}/../../../../common/rcp_sync_proto")
set(COMPONENTS main)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../../../../tools/version.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../../../../tools/test_build.cmake)
project(test_rcp_sync)
