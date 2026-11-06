# Zigbee test device for the rig: an on/off light router on a Waveshare ESP32-C6-DEV-KIT-N8.
# Derived from esp-zigbee-sdk examples/home_automation_devices/on_off_light (v2.1.0), made a router, with the example's
# external light_driver and example_common replaced by main/light.c and main/zigbee_node.c.
cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../version.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../test_build.cmake)
project(zigbee_test_device)
