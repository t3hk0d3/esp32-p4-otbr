# The C6 base image from the c6_updater build, for the projects that carry it (p4: ROM recovery; c6_installer: the
# first install through ESP-Hosted). Both must see the same images and the same version, hence one place.
#
# c6_updater_stage(<stage dir> <version out var>)
#   Fails the configure step when c6_updater was not built (tools/build_all.sh builds it first). Copies its bootloader,
#   partition table and app to <stage dir>/c6_bootloader.bin, c6_partition_table.bin and c6_updater.bin, and sets
#   <version out var> to the updater's project_version (the content-addressed base version, tools/base_version.cmake).
#   configure_file() registers each source as a configure dependency, so a rebuilt updater re-runs CMake.
set(_c6u_build "${CMAKE_CURRENT_LIST_DIR}/../c6_updater/build")

function(c6_updater_stage stage_dir version_var)
    set(inputs
        "${_c6u_build}/bootloader/bootloader.bin"
        "${_c6u_build}/partition_table/partition-table.bin"
        "${_c6u_build}/c6_updater.bin"
        "${_c6u_build}/project_description.json")
    foreach(input IN LISTS inputs)
        if(NOT EXISTS "${input}")
            # Also make CMake re-check once the file appears.
            set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${input}")
            message(FATAL_ERROR "Missing C6 build output: ${input}\n"
                                "Build the C6 projects first: run make build (tools/build_all.sh)")
        endif()
    endforeach()
    configure_file("${_c6u_build}/bootloader/bootloader.bin" "${stage_dir}/c6_bootloader.bin" COPYONLY)
    configure_file("${_c6u_build}/partition_table/partition-table.bin" "${stage_dir}/c6_partition_table.bin" COPYONLY)
    configure_file("${_c6u_build}/c6_updater.bin" "${stage_dir}/c6_updater.bin" COPYONLY)
    file(READ "${_c6u_build}/project_description.json" description)
    string(JSON version GET "${description}" project_version)
    set(${version_var} "${version}" PARENT_SCOPE)
endfunction()
