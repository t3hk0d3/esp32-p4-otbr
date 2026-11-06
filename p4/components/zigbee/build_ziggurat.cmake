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
add_custom_command(
    OUTPUT "${ZIGGURAT_LIB}" "${ZIGGURAT_HEADER}"
    COMMAND "${ziggurat_script}" "${ZIGGURAT_OUT_DIR}"
    # A new pin of the submodule changes its lock file, the script or the toolchain file change the build.
    DEPENDS "${ziggurat_src}/crates/ziggurat-ot/Cargo.lock" "${ziggurat_script}"
            "${CMAKE_CURRENT_LIST_DIR}/rust-toolchain.toml" "${CMAKE_CURRENT_LIST_FILE}"
    COMMENT "Building ziggurat (Rust staticlib)"
    VERBATIM)
add_custom_target(ziggurat_staticlib DEPENDS "${ZIGGURAT_LIB}" "${ZIGGURAT_HEADER}")

add_prebuilt_library(ziggurat_ot "${ZIGGURAT_LIB}")
add_dependencies(ziggurat_ot ziggurat_staticlib)
target_link_libraries(${COMPONENT_LIB} INTERFACE ziggurat_ot)
target_include_directories(${COMPONENT_LIB} INTERFACE "${ZIGGURAT_OUT_DIR}")
