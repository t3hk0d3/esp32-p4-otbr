idf_component_register(SRCS "test_mode_fsm.c" "test_leader.c" "test_trel_peers.c" "test_vendor_info.c"
                       REQUIRES br_core unity
                       WHOLE_ARCHIVE)
