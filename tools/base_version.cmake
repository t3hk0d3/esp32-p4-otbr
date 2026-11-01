# Sets PROJECT_VER for the frozen C6 updater (the "C6 base image") to base-<8 hex>, content-addressed:
# a SHA-1 over exactly the inputs that define the base image, so it does not depend on the build time, the git
# commit or the dirty state. The P4 embeds the same string as c6_base_version.txt; the updater reports it in
# HELLO (esp_app_desc_t.version), and a different string makes the P4 run the ROM recovery (rewrites the C6
# bootloader and partition table), so it must change only when the base image really changes.
# Inputs: c6_updater/main/*, common/rcp_sync_proto/*, common/c6_board/* (without host tests and build output),
# c6_updater/sdkconfig.defaults and the IDF version recorded in IDF_VERSION.
# Deliberately only tracked inputs: an IDF patch-level upgrade or a local menuconfig edit of the generated sdkconfig
# does not change the version.
# Include from c6_updater/CMakeLists.txt before project(). Every input is a configure dependency.
set(_bv_root "${CMAKE_CURRENT_LIST_DIR}/..")

set(_bv_files "")
foreach(_bv_dir IN ITEMS c6_updater/main common/rcp_sync_proto common/c6_board)
    file(GLOB_RECURSE _bv_found CONFIGURE_DEPENDS RELATIVE "${_bv_root}" "${_bv_root}/${_bv_dir}/*")
    list(APPEND _bv_files ${_bv_found})
endforeach()
list(APPEND _bv_files c6_updater/CMakeLists.txt c6_updater/sdkconfig.defaults IDF_VERSION)
# Editor and OS junk in the input directories is not an input.
list(FILTER _bv_files EXCLUDE REGEX "(\\.swp|\\.swo|~|\\.bak|\\.tmp|\\.DS_Store|\\.orig)$|(^|/)(\\.#|#)[^/]*$")
list(FILTER _bv_files EXCLUDE REGEX "(^|/)(test_host|build|managed_components|__pycache__)/")
list(SORT _bv_files)
list(REMOVE_DUPLICATES _bv_files)

set(_bv_manifest "")
foreach(_bv_file IN LISTS _bv_files)
    file(SHA1 "${_bv_root}/${_bv_file}" _bv_sha)
    string(APPEND _bv_manifest "${_bv_file} ${_bv_sha}\n")
    # configure_file() registers the input as a configure dependency without clashing with the entries that ESP-IDF
    # adds for its own files (CMakeLists.txt, the partition table); the copy itself is never used.
    configure_file("${_bv_root}/${_bv_file}" "${CMAKE_BINARY_DIR}/base_version_inputs/${_bv_file}" COPYONLY)
endforeach()
string(SHA1 _bv_digest "${_bv_manifest}")
string(SUBSTRING "${_bv_digest}" 0 8 _bv_short)
set(PROJECT_VER "base-${_bv_short}")
