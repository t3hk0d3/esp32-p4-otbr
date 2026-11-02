cmake_minimum_required(VERSION 3.22)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../common/rcp_sync_proto" "${CMAKE_CURRENT_LIST_DIR}/../common/c6_board"
                         "${CMAKE_CURRENT_LIST_DIR}/../common/board_cores"
                         # esp_ot_br_server includes protocol_examples_common.h. Taken from the IDF tree here, not as a
                         # manifest path dependency: dependencies.lock would record that path relative to this machine.
                         "$ENV{IDF_PATH}/examples/common_components/protocol_examples_common")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../tools/version.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../tools/test_build.cmake)
# Trim the build to main and what it depends on; later tasks add components to main's REQUIRES.
idf_build_set_property(MINIMAL_BUILD ON)
project(p4)

# --- C6 images carried by the P4 (spec 4.2, 5.1, 5.5) ---
# The C6 projects are built first by tools/build_all.sh. Their outputs are staged into the P4
# build tree at configure time; configure_file() registers each source as a configure dependency,
# so a rebuilt C6 image re-runs CMake and refreshes everything below.
set(c6_updater_build "${CMAKE_CURRENT_LIST_DIR}/../c6_updater/build")
set(c6_migrator_build "${CMAKE_CURRENT_LIST_DIR}/../c6_migrator/build")
set(c6_inputs
    "${c6_updater_build}/bootloader/bootloader.bin"
    "${c6_updater_build}/partition_table/partition-table.bin"
    "${c6_updater_build}/c6_updater.bin"
    "${c6_updater_build}/project_description.json"
    "${c6_migrator_build}/c6_migrator.bin")
foreach(input IN LISTS c6_inputs)
    if(NOT EXISTS "${input}")
        # Also make CMake re-check once the file appears.
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${input}")
        message(FATAL_ERROR "Missing C6 build output: ${input}\n"
                            "Build the C6 projects first: run make build (tools/build_all.sh)")
    endif()
endforeach()

# C6 base image: embedded in the P4 app (see main/c6_base_image.h).
set(c6_stage "${CMAKE_BINARY_DIR}/c6_images")
configure_file("${c6_updater_build}/bootloader/bootloader.bin" "${c6_stage}/c6_bootloader.bin" COPYONLY)
configure_file("${c6_updater_build}/partition_table/partition-table.bin" "${c6_stage}/c6_partition_table.bin" COPYONLY)
configure_file("${c6_updater_build}/c6_updater.bin" "${c6_stage}/c6_updater.bin" COPYONLY)
# The installer the P4 sends through the factory ESP-Hosted OTA on the first radio firmware upload (c6_hosted).
configure_file("${c6_migrator_build}/c6_migrator.bin" "${c6_stage}/c6_migrator.bin" COPYONLY)
file(READ "${c6_updater_build}/project_description.json" c6_updater_description)
string(JSON c6_updater_version GET "${c6_updater_description}" project_version)
set(c6_version_file "${c6_stage}/c6_base_version.txt")
set(c6_version_old "")
if(EXISTS "${c6_version_file}")
    file(READ "${c6_version_file}" c6_version_old)
endif()
if(NOT c6_version_old STREQUAL c6_updater_version)  # leave the file alone when unchanged
    file(WRITE "${c6_version_file}" "${c6_updater_version}")
endif()

# The RCP firmware is not part of the P4 image (R14): it is pushed to the C6 on request. The web UI -> web_0.
spiffs_create_partition_image(web_0 "${CMAKE_CURRENT_LIST_DIR}/components/esp_ot_br_server/frontend"
                              FLASH_IN_PROJECT)
