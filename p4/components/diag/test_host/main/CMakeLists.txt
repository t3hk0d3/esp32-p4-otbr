idf_component_register(SRCS "test_windows.c" "test_lock_stats.c" "test_snapshot_json.c"
                       REQUIRES diag unity
                       WHOLE_ARCHIVE)
