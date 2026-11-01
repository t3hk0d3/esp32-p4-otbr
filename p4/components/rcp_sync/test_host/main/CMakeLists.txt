idf_component_register(SRCS "test_rcp_sync.c" "test_recovery.c" "test_rom_plan.c" "test_log_ring.c" "test_log_limiter.c" "test_uart_handoff.c"
                       REQUIRES rcp_sync rcp_sync_proto unity
                       WHOLE_ARCHIVE)
target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../../../../common/c6_board")
