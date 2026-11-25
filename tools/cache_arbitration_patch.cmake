# P4 v3.x L1 D-cache arbitration livelock (ESP-IDF issue 19369, https://github.com/espressif/esp-idf/issues/19369). A
# generated "hal" component in the build directory overrides ESP-IDF's: symlinks to IDF's files and a patched copy of
# cache_hal.c (PATCHES.md, "Generated ESP-IDF copies"):
#   cache_hal.c: cache_hal_init() sets the cache preload strategy of every cache to CACHE_LL_PRELOAD_ARBITRARY (2). On
#     chips of revision v3.0 and later that selects random arbitration, and Espressif confirmed that with it one core's
#     spin on a cacheable address can starve the other core's L1 D-cache miss: on our boards, CPU1's wait for FreeRTOS
#     starves CPU0 into a Store access fault on the first boot after a flash write (and rarely at runtime). Per
#     Espressif only the L1 D-cache field (l1_dcache_undef_op) is affected, so a line right after IDF's sets just that
#     one to CACHE_LL_PRELOAD_UNTIL_FETCH_DONE (0, round-robin); the L1 I-cache and L2 keep IDF's 2. This costs some
#     performance where the D-cache preloads. The level is 1 because the P4 setter compares cache_level == 1; on
#     ESP32-P4-rev1.3 builds the setter compiles to nothing, so those images are unchanged.
# The app calls cache_hal_init() before it starts CPU1, so the value is set before any spin. The bootloader keeps IDF's
# own hal (only one core runs there). A text that is not there any more stops the configure, naming the IDF file. Drop
# this script once ESP-IDF sets round-robin itself or fixes mode 2. The IDF tree itself stays untouched.
# Include from p4/CMakeLists.txt after IDF's project.cmake and before project().

set(cache_patch_idf "$ENV{IDF_PATH}/components/hal")
set(cache_patch_shadow "${CMAKE_BINARY_DIR}/idf_patch_components/hal")

# Symlinks in `dst` to every entry of `src` except those named in the remaining arguments. The listing of `src` is a
# configure dependency (an IDF update that adds or removes a file there reconfigures), and a link whose target is gone
# is removed (a file IDF no longer has must not stay in the generated component).
function(cache_patch_link_all src dst)
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
function(cache_patch_replace var file old new)
    string(FIND "${${var}}" "${old}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "cache_arbitration_patch: a text to replace is not in ${file} any more:\n${old}")
    endif()
    string(REPLACE "${old}" "${new}" text "${${var}}")
    set(${var} "${text}" PARENT_SCOPE)
endfunction()

# Writes only when the content changed, so a reconfigure does not rebuild what did not change.
function(cache_patch_write path text)
    if(EXISTS "${path}" AND NOT IS_SYMLINK "${path}")
        file(READ "${path}" old)
        if(old STREQUAL text)
            return()
        endif()
    endif()
    file(REMOVE "${path}")
    file(WRITE "${path}" "${text}")
endfunction()

cache_patch_link_all("${cache_patch_idf}" "${cache_patch_shadow}" cache_hal.c)

# --- cache_hal.c ---
set(cache_patch_file "${cache_patch_idf}/cache_hal.c")
file(READ "${cache_patch_file}" cache_patch_text)
cache_patch_replace(cache_patch_text "${cache_patch_file}"
    [=[    cache_ll_preload_set_strategy(CACHE_LL_LEVEL_ALL, CACHE_TYPE_ALL, CACHE_LL_ID_ALL, CACHE_LL_PRELOAD_ARBITRARY);
]=]
    [=[    cache_ll_preload_set_strategy(CACHE_LL_LEVEL_ALL, CACHE_TYPE_ALL, CACHE_LL_ID_ALL, CACHE_LL_PRELOAD_ARBITRARY);
    // cache_arbitration_patch: ESP-IDF issue 19369, random L1 D-cache arbitration livelocks the cores on P4 v3.x
    cache_ll_preload_set_strategy(1, CACHE_TYPE_DATA, CACHE_LL_ID_ALL, CACHE_LL_PRELOAD_UNTIL_FETCH_DONE);
]=])
cache_patch_write("${cache_patch_shadow}/cache_hal.c" "${cache_patch_text}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${cache_patch_file}" "${CMAKE_CURRENT_LIST_FILE}")

list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_BINARY_DIR}/idf_patch_components")
message(STATUS "cache_arbitration_patch: hal from ${cache_patch_shadow} (round-robin L1 D-cache, IDF issue 19369)")
