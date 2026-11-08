if(IDF_TARGET STREQUAL "linux")
    # Host tests cover the mode state machine, the leader weight logic (leader.c), the TREL peer sorter and the vendor
    # version helper and the RCP probe's frames; they need nothing from OpenThread but a few types (stubbed).
    idf_component_register(SRCS "mode_fsm.c" "leader.c" "trel_peers.c" "vendor_info.c" "otbr_probe.c"
                           INCLUDE_DIRS "include" "." "test_host/stubs"
                           REQUIRES rcp_sync
                           PRIV_REQUIRES log radio_mux) # radio_mux: radio_mux_spinel_uint.h (otbr_probe.c)
    return()
endif()

set(srcs "br_core.c" "br_core_radio.c" "mode_fsm.c" "validity.c" "leader.c" "trel_peers.c" "vendor_info.c" "otbr_probe.c")

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "include"
                       REQUIRES esp_netif rcp_sync
                       PRIV_REQUIRES board_cores openthread esp_ot_br_server mdns esp_event esp_timer esp_hw_support console
                                     esp_driver_gpio app_update vfs nvs_flash log radio_mux builtin_rcp)
# board.h lives in main (which depends on this component); include it by path.
target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../main")
