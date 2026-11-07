idf_component_register(SRCS "test_attach.c" "test_core.c" "test_detect.c" "test_hdlc.c" "test_pool.c" "test_spi.c"
                            "test_spinel_uint.c"
                       REQUIRES radio_mux unity
                       WHOLE_ARCHIVE)
