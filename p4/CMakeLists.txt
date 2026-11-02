cmake_minimum_required(VERSION 3.22)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../common/rcp_sync_proto" "${CMAKE_CURRENT_LIST_DIR}/../common/c6_board"
                         "${CMAKE_CURRENT_LIST_DIR}/../common/board_cores"
                         # esp_ot_br_server includes protocol_examples_common.h. Taken from the IDF tree here, not as a
                         # manifest path dependency: dependencies.lock would record that path relative to this machine.
                         "$ENV{IDF_PATH}/examples/common_components/protocol_examples_common")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../tools/version.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../tools/test_build.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../tools/c6_updater_images.cmake)
# Trim the build to main and what it depends on; later tasks add components to main's REQUIRES.
idf_build_set_property(MINIMAL_BUILD ON)
project(p4)

# --- C6 images carried by the P4 (spec 4.2, 5.1, 5.5) ---
# The C6 projects are built first by tools/build_all.sh. Their outputs are staged into the P4
# build tree at configure time; configure_file() registers each source as a configure dependency,
# so a rebuilt C6 image re-runs CMake and refreshes everything below.
# C6 base image: embedded in the P4 app (see main/c6_base_image.h).
set(c6_stage "${CMAKE_BINARY_DIR}/c6_images")
c6_updater_stage("${c6_stage}" c6_updater_version)
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
