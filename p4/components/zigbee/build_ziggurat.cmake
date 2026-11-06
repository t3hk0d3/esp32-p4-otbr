# Builds ziggurat's staticlib and header with tools/build_staticlib.sh into <build>/ziggurat and wraps the archive as a
# prebuilt library, target ziggurat_ot. Include from the component's CMakeLists.txt after idf_component_register().
# Outputs: ${ZIGGURAT_OUT_DIR}/libziggurat_ot.a, ${ZIGGURAT_OUT_DIR}/ziggurat.h
# The archive imports the ziggurat_platform_* functions and zigbee_heap_aligned_alloc / zigbee_heap_realloc /
# zigbee_heap_free; the component's C code provides them.
set(ZIGGURAT_OUT_DIR "${CMAKE_BINARY_DIR}/ziggurat")
set(ZIGGURAT_LIB "${ZIGGURAT_OUT_DIR}/libziggurat_ot.a")
set(ZIGGURAT_HEADER "${ZIGGURAT_OUT_DIR}/ziggurat.h")
set(ziggurat_src "${CMAKE_CURRENT_LIST_DIR}/ziggurat")
set(ziggurat_script "${CMAKE_CURRENT_LIST_DIR}/tools/build_staticlib.sh")

if(NOT EXISTS "${ziggurat_src}/crates/ziggurat-ot/Cargo.toml")
    message(FATAL_ERROR "The ziggurat submodule is empty: git submodule update --init")
endif()

file(MAKE_DIRECTORY "${ZIGGURAT_OUT_DIR}")

# What the cargo step reads. The globs are re-checked on every build (CONFIGURE_DEPENDS), so a new or removed source file
# reconfigures. The submodule's recorded commit is stamped too: a pin bump or a checkout moves its HEAD, which is a
# configure dependency, and the stamp is rewritten only when the commit differs (a local edit shows as a changed source).
file(GLOB_RECURSE ziggurat_inputs CONFIGURE_DEPENDS
     "${ziggurat_src}/crates/*/src/*.rs" "${ziggurat_src}/crates/*/build.rs" "${ziggurat_src}/crates/*/Cargo.toml"
     "${ziggurat_src}/crates/*/Cargo.lock" "${ziggurat_src}/crates/ziggurat-ot/cbindgen.toml"
     "${ziggurat_src}/Cargo.toml" "${ziggurat_src}/Cargo.lock")
execute_process(COMMAND git -C "${ziggurat_src}" rev-parse HEAD
                OUTPUT_VARIABLE ziggurat_commit OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE ziggurat_rc ERROR_QUIET)
execute_process(COMMAND git -C "${ziggurat_src}" rev-parse --absolute-git-dir
                OUTPUT_VARIABLE ziggurat_gitdir OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT ziggurat_rc EQUAL 0 OR ziggurat_commit STREQUAL "")
    message(FATAL_ERROR "build_ziggurat.cmake: 'git rev-parse HEAD' failed in the ziggurat submodule")
endif()
set(ziggurat_stamp "${ZIGGURAT_OUT_DIR}/commit.txt")
set(ziggurat_stamp_old "")
if(EXISTS "${ziggurat_stamp}")
    file(READ "${ziggurat_stamp}" ziggurat_stamp_old)
endif()
if(NOT ziggurat_stamp_old STREQUAL "${ziggurat_commit}\n")
    file(WRITE "${ziggurat_stamp}" "${ziggurat_commit}\n")
endif()
if(EXISTS "${ziggurat_gitdir}/HEAD")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${ziggurat_gitdir}/HEAD")
endif()

add_custom_command(
    OUTPUT "${ZIGGURAT_LIB}" "${ZIGGURAT_HEADER}"
    COMMAND "${ziggurat_script}" "${ZIGGURAT_OUT_DIR}"
    # The submodule's commit and its Rust sources and manifests, the script, the toolchain file and this file.
    DEPENDS "${ziggurat_stamp}" ${ziggurat_inputs} "${ziggurat_script}"
            "${CMAKE_CURRENT_LIST_DIR}/rust-toolchain.toml" "${CMAKE_CURRENT_LIST_FILE}"
    COMMENT "Building ziggurat (Rust staticlib)"
    VERBATIM)
add_custom_target(ziggurat_staticlib DEPENDS "${ZIGGURAT_LIB}" "${ZIGGURAT_HEADER}")

add_prebuilt_library(ziggurat_ot "${ZIGGURAT_LIB}")
add_dependencies(ziggurat_ot ziggurat_staticlib)
target_link_libraries(${COMPONENT_LIB} INTERFACE ziggurat_ot)
target_include_directories(${COMPONENT_LIB} INTERFACE "${ZIGGURAT_OUT_DIR}")
