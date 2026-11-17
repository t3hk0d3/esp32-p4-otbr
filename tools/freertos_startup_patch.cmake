# P4 v3.x first boot after a flash write (ESP-IDF issue 19369, https://github.com/espressif/esp-idf/issues/19369). A
# generated "freertos" component in the build directory overrides ESP-IDF's: symlinks to IDF's files and a patched copy
# of app_startup.c (PATCHES.md, "Generated ESP-IDF copies"):
#   app_startup.c: while CPU0 starts FreeRTOS, CPU1 waits in esp_startup_start_app_other_cores() in an empty loop. On a
#     P4 v3.2 that spin starves CPU0's cache accesses, and CPU0's stack memset in prvInitialiseNewTask gets a Store
#     access fault (MCAUSE 7, MEPC 0x4fc1a572) on the first boot after a flash write; after an OTA the bootloader then
#     rolls the update back. The loop body becomes esp_rom_delay_us(10) (first boots: 4 of 4 panicked before, 0 of 3
#     after, on the NANO-DB).
# Every replaced text is looked up first; one that is not there any more stops the configure, naming the IDF file. Drop
# this script once ESP-IDF fixes the loop. The IDF tree itself stays untouched.
# Include from p4/CMakeLists.txt after IDF's project.cmake and before project().

set(freertos_idf "$ENV{IDF_PATH}/components/freertos")
set(freertos_shadow "${CMAKE_BINARY_DIR}/idf_patch_components/freertos")

# Symlinks in `dst` to every entry of `src` except those named in the remaining arguments. The listing of `src` is a
# configure dependency (an IDF update that adds or removes a file there reconfigures), and a link whose target is gone
# is removed (a file IDF no longer has must not stay in the generated component).
function(freertos_patch_link_all src dst)
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
function(freertos_patch_replace var file old new)
    string(FIND "${${var}}" "${old}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "freertos_startup_patch: a text to replace is not in ${file} any more:\n${old}")
    endif()
    string(REPLACE "${old}" "${new}" text "${${var}}")
    set(${var} "${text}" PARENT_SCOPE)
endfunction()

# Writes only when the content changed, so a reconfigure does not rebuild what did not change.
function(freertos_patch_write path text)
    if(EXISTS "${path}" AND NOT IS_SYMLINK "${path}")
        file(READ "${path}" old)
        if(old STREQUAL text)
            return()
        endif()
    endif()
    file(REMOVE "${path}")
    file(WRITE "${path}" "${text}")
endfunction()

freertos_patch_link_all("${freertos_idf}" "${freertos_shadow}" app_startup.c)

# --- app_startup.c ---
set(freertos_file "${freertos_idf}/app_startup.c")
file(READ "${freertos_file}" freertos_text)
# After the last include outside an #if, so the delay is declared in every configuration.
freertos_patch_replace(freertos_text "${freertos_file}"
    [=[#include "esp_chip_info.h"
]=]
    [=[#include "esp_chip_info.h"
#include "esp_rom_sys.h" // freertos_startup_patch: esp_rom_delay_us
]=])
freertos_patch_replace(freertos_text "${freertos_file}"
    [=[    while (port_xSchedulerRunning[0] == 0) {
        ;
    }]=]
    [=[    while (port_xSchedulerRunning[0] == 0) {
        esp_rom_delay_us(10); // freertos_startup_patch: ESP-IDF issue 19369, CPU1's spin starves CPU0 on P4 v3.x
    }]=])
freertos_patch_write("${freertos_shadow}/app_startup.c" "${freertos_text}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${freertos_file}" "${CMAKE_CURRENT_LIST_FILE}")

list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_BINARY_DIR}/idf_patch_components")
message(STATUS "freertos_startup_patch: FreeRTOS from ${freertos_shadow} (pause in CPU1's start wait, IDF issue 19369)")
