cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/.." "${CMAKE_CURRENT_LIST_DIR}/../../rcp_sync"
                         "${CMAKE_CURRENT_LIST_DIR}/../../netnames" "${CMAKE_CURRENT_LIST_DIR}/../../br_core"
                         "${CMAKE_CURRENT_LIST_DIR}/../../netcfg"
                         "${CMAKE_CURRENT_LIST_DIR}/../../routing"
                         "${CMAKE_CURRENT_LIST_DIR}/../../radio_mux" "${CMAKE_CURRENT_LIST_DIR}/../../rcp_proxy" "${CMAKE_CURRENT_LIST_DIR}/../../zigbee"
                         "${CMAKE_CURRENT_LIST_DIR}/../../self_update"
                         "${CMAKE_CURRENT_LIST_DIR}/../../timesync"
                         "${CMAKE_CURRENT_LIST_DIR}/../../../../common/rcp_sync_proto")
set(COMPONENTS main)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../../../../tools/version.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../../../../tools/test_build.cmake)
project(test_device_api)
