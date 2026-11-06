cmake_minimum_required(VERSION 3.16)

set(COMPONENTS main)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../../../tools/version.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../../../tools/test_build.cmake)
project(test_ot_port)
