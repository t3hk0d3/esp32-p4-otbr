# p4/ot_port is not a component (its files are compiled into the generated openthread component): the pure helper is
# built here directly, with spinel.h from ESP-IDF's OpenThread.
idf_component_register(SRCS "test_mux_gone_answer.c" "../../mux_gone_answer.c"
                       INCLUDE_DIRS "../.." "../../../components/radio_mux/include"
                                    "$ENV{IDF_PATH}/components/openthread/openthread/src/lib/spinel"
                       REQUIRES unity
                       WHOLE_ARCHIVE)
