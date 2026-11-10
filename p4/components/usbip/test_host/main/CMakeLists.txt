idf_component_register(SRCS "test_proto.c" "test_core.c" "test_budget.c"
                       REQUIRES usbip unity
                       WHOLE_ARCHIVE)
