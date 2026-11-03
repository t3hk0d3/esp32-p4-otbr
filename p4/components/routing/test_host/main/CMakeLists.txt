idf_component_register(SRCS "test_settings.c" "test_announce.c" "test_bgp_msg.c"
                       REQUIRES routing unity
                       WHOLE_ARCHIVE)
