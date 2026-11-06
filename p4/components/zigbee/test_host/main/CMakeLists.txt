idf_component_register(SRCS "test_ccm.c" "test_heap_stats.c" "test_link.c" "test_spinel.c" "test_main.c"
                       PRIV_INCLUDE_DIRS "${IDF_PATH}/components/openthread/openthread/src/lib/spinel"
                       REQUIRES zigbee unity
                       WHOLE_ARCHIVE)
