# c6_installer: the one-time C6 app for a factory board. The P4 sends it through the factory ESP-Hosted OTA (c6_hosted);
# the stock bootloader starts it, it writes our bootloader, partition table and updater (embedded below, from the
# c6_updater build), then restarts into the updater. The RCP is uploaded afterwards as usual.
cmake_minimum_required(VERSION 3.22)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../common/c6_board")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../tools/test_build.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../tools/c6_updater_images.cmake)

# The base image this installer writes, staged where main/CMakeLists.txt embeds it. Its version is the installer's: the P4
# sends the installer only when that equals its own embedded base version.
c6_updater_stage("${CMAKE_BINARY_DIR}/c6_images" PROJECT_VER)

idf_build_set_property(MINIMAL_BUILD ON)
project(c6_installer)
