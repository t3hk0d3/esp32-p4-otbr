idf_component_register(SRCS "test_ccm.c" "test_heap_stats.c" "test_kick.c" "test_zlog_map.c" "test_link.c" "test_spinel.c" "test_main.c"
                       REQUIRES zigbee unity
                       WHOLE_ARCHIVE)
# The tests build spinel frames with spinel.h; added after the registration so ESP-IDF does not warn that the
# directory belongs to the openthread component.
target_include_directories(${COMPONENT_LIB} PRIVATE "${IDF_PATH}/components/openthread/openthread/src/lib/spinel")
