idf_component_register(SRCS "test_mode_fsm.c" "test_leader.c" "test_trel_peers.c" "test_vendor_info.c" "test_otbr_probe.c" "test_ot_log_map.c"
                       REQUIRES br_core radio_mux unity
                       WHOLE_ARCHIVE)
