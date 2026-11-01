idf_component_register(SRCS "test_config.c" "test_policy.c" "test_trial.c" "test_vlan.c" "test_isolation.c" "test_mdns.c"
                       REQUIRES netcfg unity
                       WHOLE_ARCHIVE)
