# Turns the CMake variables OTBR_BOARD_ID and OTBR_CHIP_ID into the C string macros OTBR_BOARD_ID and OTBR_CHIP_ID for all
# components. Include from p4's top-level CMakeLists.txt after IDF's project.cmake and before project().
# tools/build_all.sh and the Makefile pass both on every run (from tools/targets.py); the configure step fails without
# them, so a build never carries an unnamed target.
foreach(_target_var OTBR_BOARD_ID OTBR_CHIP_ID)
    if("${${_target_var}}" STREQUAL "")
        message(FATAL_ERROR "target.cmake: ${_target_var} is not set; build with make build or tools/build_all.sh")
    endif()
    idf_build_set_property(COMPILE_DEFINITIONS "${_target_var}=\"${${_target_var}}\"" APPEND)
endforeach()
