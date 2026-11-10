idf_component_register(SRCS "test_core.c" "test_text.c" "test_ring.c" "test_settings.c" "test_drain.c" "test_viewers.c" "test_console.c" "test_main.c"
                       REQUIRES logging unity freertos log
                       WHOLE_ARCHIVE)
