idf_component_register(SRCS "test_attach.c" "test_core.c" "test_detect.c" "test_hdlc.c" "test_pool.c"
                       REQUIRES radio_mux unity
                       WHOLE_ARCHIVE)
