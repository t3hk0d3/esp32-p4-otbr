idf_component_register(SRCS "test_proto.c" "test_core.c" "test_budget.c" "test_engine.c"
                       REQUIRES usbip unity
                       WHOLE_ARCHIVE)
