# c6_installer: the one-time C6 app for a factory board. The P4 sends it through the factory ESP-Hosted OTA (c6_hosted);
# the stock bootloader starts it, it writes our bootloader, partition table and updater (embedded below, from the
# c6_updater build), then restarts into the updater. The RCP is uploaded afterwards as usual.
cmake_minimum_required(VERSION 3.22)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../common/c6_board")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../tools/test_build.cmake)

# The c6_updater build is an input (tools/build_all.sh builds it first). Staged at configure time; configure_file()
# registers each source as a configure dependency, so a rebuilt updater re-runs CMake.
set(c6_updater_build "${CMAKE_CURRENT_LIST_DIR}/../c6_updater/build")
set(c6_inputs
    "${c6_updater_build}/bootloader/bootloader.bin"
    "${c6_updater_build}/partition_table/partition-table.bin"
    "${c6_updater_build}/c6_updater.bin"
    "${c6_updater_build}/project_description.json")
foreach(input IN LISTS c6_inputs)
    if(NOT EXISTS "${input}")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${input}")
        message(FATAL_ERROR "Missing C6 build output: ${input}\n"
                            "Build c6_updater first: run make build (tools/build_all.sh)")
    endif()
endforeach()
set(c6_stage "${CMAKE_BINARY_DIR}/c6_images")
configure_file("${c6_updater_build}/bootloader/bootloader.bin" "${c6_stage}/c6_bootloader.bin" COPYONLY)
configure_file("${c6_updater_build}/partition_table/partition-table.bin" "${c6_stage}/c6_partition_table.bin" COPYONLY)
configure_file("${c6_updater_build}/c6_updater.bin" "${c6_stage}/c6_updater.bin" COPYONLY)

# The installer's version is the base version it installs: the P4 sends it only when that equals its own embedded one.
file(READ "${c6_updater_build}/project_description.json" c6_updater_description)
string(JSON PROJECT_VER GET "${c6_updater_description}" project_version)

idf_build_set_property(MINIMAL_BUILD ON)
project(c6_installer)
