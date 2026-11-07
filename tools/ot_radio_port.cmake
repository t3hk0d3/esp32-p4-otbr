# OpenThread on any radio mux channel (spec section 3, OTBR mode). A generated "openthread" component in the build
# directory overrides ESP-IDF's: symlinks to IDF's files, the project's MuxSpinelInterface (p4/ot_port) in src/port and
# include, and patched copies of two IDF files (PATCHES.md, "Generated ESP-IDF copies"):
#   src/port/esp_openthread_radio_spinel.cpp: the static spinel interface is the MuxSpinelInterface, the radio caps
#     OpenThread requires from the RCP are the Global Constraints set (the ot-br-posix set for Thread 1.2+), and
#     otPlatRadioGetCaps no longer overwrites that set with the running RCP's caps;
#   CMakeLists.txt: the git hash of the package version is read from IDF's component, not from the build directory
#     (which lies in this repository and would change it, and rebuild OpenThread, on every commit).
# Every replaced text is looked up first; one that is not there any more stops the configure, naming the IDF file.
# Kconfig stays CONFIG_OPENTHREAD_RADIO_SPINEL_SPI. The IDF tree itself stays untouched.
# Include from p4/CMakeLists.txt after IDF's project.cmake and before project().

set(ot_idf "$ENV{IDF_PATH}/components/openthread")
set(ot_port_src "${CMAKE_CURRENT_LIST_DIR}/../p4/ot_port")
set(ot_shadow "${CMAKE_BINARY_DIR}/ot_port_components/openthread")

# Symlinks in `dst` to every entry of `src` except those named in the remaining arguments. The listing of `src` is a
# configure dependency (an IDF update that adds or removes a file there reconfigures), and a link whose target is gone
# is removed (a file IDF no longer has must not stay in the generated component).
function(ot_port_link_all src dst)
    file(MAKE_DIRECTORY "${dst}")
    file(GLOB old LIST_DIRECTORIES true RELATIVE "${dst}" "${dst}/*")
    foreach(entry ${old})
        if(IS_SYMLINK "${dst}/${entry}" AND NOT EXISTS "${dst}/${entry}")
            file(REMOVE "${dst}/${entry}")
        endif()
    endforeach()
    file(GLOB entries LIST_DIRECTORIES true CONFIGURE_DEPENDS RELATIVE "${src}" "${src}/*")
    foreach(entry ${entries})
        if(NOT entry IN_LIST ARGN)
            file(CREATE_LINK "${src}/${entry}" "${dst}/${entry}" SYMBOLIC)
        endif()
    endforeach()
endfunction()

# Replaces `old` by `new` in the variable named `var`; a FATAL_ERROR naming `file` when `old` is not in it.
function(ot_port_patch var file old new)
    string(FIND "${${var}}" "${old}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "ot_radio_port: a text to replace is not in ${file} any more:\n${old}")
    endif()
    string(REPLACE "${old}" "${new}" text "${${var}}")
    set(${var} "${text}" PARENT_SCOPE)
endfunction()

# Writes only when the content changed, so a reconfigure does not rebuild what did not change.
function(ot_port_write path text)
    if(EXISTS "${path}" AND NOT IS_SYMLINK "${path}")
        file(READ "${path}" old)
        if(old STREQUAL text)
            return()
        endif()
    endif()
    file(REMOVE "${path}")
    file(WRITE "${path}" "${text}")
endfunction()

ot_port_link_all("${ot_idf}" "${ot_shadow}" src include CMakeLists.txt)
ot_port_link_all("${ot_idf}/src" "${ot_shadow}/src" port)
ot_port_link_all("${ot_idf}/src/port" "${ot_shadow}/src/port" esp_openthread_radio_spinel.cpp)
ot_port_link_all("${ot_idf}/include" "${ot_shadow}/include")
file(CREATE_LINK "${ot_port_src}/mux_spinel_interface.cpp" "${ot_shadow}/src/port/mux_spinel_interface.cpp" SYMBOLIC)
file(CREATE_LINK "${ot_port_src}/mux_spinel_interface.hpp" "${ot_shadow}/src/port/mux_spinel_interface.hpp" SYMBOLIC)
file(CREATE_LINK "${ot_port_src}/mux_gone_answer.c" "${ot_shadow}/src/port/mux_gone_answer.c" SYMBOLIC)
file(CREATE_LINK "${ot_port_src}/mux_gone_answer.h" "${ot_shadow}/src/port/mux_gone_answer.h" SYMBOLIC)
# The spinel packed-uint helper of radio_mux (header-only), next to mux_gone_answer.c that includes it: the generated
# component does not depend on radio_mux.
file(CREATE_LINK "${CMAKE_CURRENT_LIST_DIR}/../p4/components/radio_mux/include/radio_mux_spinel_uint.h"
     "${ot_shadow}/src/port/radio_mux_spinel_uint.h" SYMBOLIC)
file(CREATE_LINK "${ot_port_src}/esp_openthread_mux.h" "${ot_shadow}/include/esp_openthread_mux.h" SYMBOLIC)

# --- src/port/esp_openthread_radio_spinel.cpp ---
set(ot_file "${ot_idf}/src/port/esp_openthread_radio_spinel.cpp")
file(READ "${ot_file}" ot_text)
ot_port_patch(ot_text "${ot_file}"
    [=[#include "esp_spi_spinel_interface.hpp"
]=]
    [=[#include "esp_spi_spinel_interface.hpp"
#include "mux_spinel_interface.hpp" // ot_radio_port: OpenThread's radio through the radio mux
]=])
ot_port_patch(ot_text "${ot_file}"
    [=[using esp::openthread::SpiSpinelInterface;
static SpinelInterfaceAdapter<SpiSpinelInterface> s_spinel_interface;]=]
    [=[using esp::openthread::MuxSpinelInterface; // ot_radio_port: in place of SpiSpinelInterface
static SpinelInterfaceAdapter<MuxSpinelInterface> s_spinel_interface;]=])
ot_port_patch(ot_text "${ot_file}"
    [=[static otRadioCaps s_radio_caps = (OT_RADIO_CAPS_ENERGY_SCAN       |
#if CONFIG_OPENTHREAD_RX_ON_WHEN_IDLE
                                   OT_RADIO_CAPS_RX_ON_WHEN_IDLE   |
#endif
                                   OT_RADIO_CAPS_TRANSMIT_SEC      |
                                   OT_RADIO_CAPS_RECEIVE_TIMING    |
                                   OT_RADIO_CAPS_TRANSMIT_TIMING   |
                                   OT_RADIO_CAPS_ACK_TIMEOUT       |
                                   OT_RADIO_CAPS_SLEEP_TO_TX       |
                                   OT_RADIO_CAPS_CSMA_BACKOFF      |
                                   OT_RADIO_CAPS_TRANSMIT_RETRIES);]=]
    [=[// ot_radio_port: the caps required from the RCP are the ot-br-posix set for Thread 1.2+ (Global Constraints);
// energy scan, sleep-to-tx, rx timing and rx-on-when-idle are used when the RCP has them, never required.
static otRadioCaps s_radio_caps = (OT_RADIO_CAPS_ACK_TIMEOUT       |
                                   OT_RADIO_CAPS_TRANSMIT_RETRIES  |
                                   OT_RADIO_CAPS_CSMA_BACKOFF      |
                                   OT_RADIO_CAPS_TRANSMIT_SEC      |
                                   OT_RADIO_CAPS_TRANSMIT_TIMING);]=])
ot_port_patch(ot_text "${ot_file}"
    [=[    s_radio_caps = s_radio.GetRadioCaps();
    return s_radio_caps;]=]
    [=[    // ot_radio_port: s_radio_caps stays the required set; the next esp_openthread_init (another RCP after a move)
    // must not require what the previous RCP had.
    return s_radio.GetRadioCaps();]=])
ot_port_write("${ot_shadow}/src/port/esp_openthread_radio_spinel.cpp" "${ot_text}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${ot_file}")

# --- CMakeLists.txt ---
set(ot_file "${ot_idf}/CMakeLists.txt")
file(READ "${ot_file}" ot_text)
ot_port_patch(ot_text "${ot_file}"
    [=[    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE IDF_VERSION_FOR_OPENTHREAD_PACKAGE]=]
    "    WORKING_DIRECTORY \"${ot_idf}\" # ot_radio_port: IDF's git, not this build directory's
    OUTPUT_VARIABLE IDF_VERSION_FOR_OPENTHREAD_PACKAGE")
ot_port_write("${ot_shadow}/CMakeLists.txt" "${ot_text}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${ot_file}" "${CMAKE_CURRENT_LIST_FILE}")

list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_BINARY_DIR}/ot_port_components")
message(STATUS "ot_radio_port: OpenThread from ${ot_shadow} (spinel over the radio mux)")
