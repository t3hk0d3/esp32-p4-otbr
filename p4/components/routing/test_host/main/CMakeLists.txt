idf_component_register(SRCS "test_settings.c" "test_announce.c" "test_bgp_msg.c" "test_bgp_fsm.c" "test_ripng.c"
                            "test_publisher.c" "test_engine.c" "fake_ops.c" "fake_io.c"
                       REQUIRES routing unity
                       WHOLE_ARCHIVE)
