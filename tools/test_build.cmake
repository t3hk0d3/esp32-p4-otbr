# Turns the OTBR_TEST_BUILD CMake variable into the C macro OTBR_TEST_BUILD=1 for all components.
# Include from a project's top-level CMakeLists.txt after IDF's project.cmake and before project().
# tools/build_all.sh sets OTBR_TEST_BUILD to 0 or 1 on every run, so the cached value never sticks.
if(OTBR_TEST_BUILD)
    idf_build_set_property(COMPILE_DEFINITIONS "OTBR_TEST_BUILD=1" APPEND)
    message(STATUS "OTBR_TEST_BUILD enabled")
endif()
