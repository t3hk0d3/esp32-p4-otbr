# Zigbee mode: ziggurat's prebuilt staticlib on a radio_mux channel, its own task, and the WebSocket /zigbee.
#
# The spinel client encodes and decodes with OpenThread's plain-C lib/spinel/spinel.c. On the target the openthread
# component already builds it (into its own library), so the client links that copy; the linux target has no
# openthread component, so the host build compiles the file by path. Neither goes through idf_component_register's
# SRCS / PRIV_INCLUDE_DIRS (which would make ESP-IDF warn that the file and the directory belong to openthread):
# target_sources / target_include_directories add them after the registration.
set(spinel_dir "${IDF_PATH}/components/openthread/openthread/src/lib/spinel")
set(pure_srcs "ccm.c" "heap_stats.c" "kick.c" "link.c" "spinel_client.c" "zlog_map.c" "zlog_ring.c")

if(IDF_TARGET STREQUAL "linux")
    # Host tests build the pure files only; the cargo step needs the riscv toolchain and stays off the host.
    idf_component_register(SRCS ${pure_srcs}
                           INCLUDE_DIRS "include")
    target_sources(${COMPONENT_LIB} PRIVATE "${spinel_dir}/spinel.c")
    target_include_directories(${COMPONENT_LIB} PRIVATE "${spinel_dir}")
    return()
endif()

# openthread: only for spinel.c (encode/decode); nothing here calls OpenThread. lwip: the client's address and the
# send-buffer check, on the HTTP server task (ws.c). board_cores: task placement. reboot_gate: the client's hard reset.
# netcfg: the WebSocket's per-interface policy.
idf_component_register(SRCS ${pure_srcs} "zigbee.c" "platform.c" "ws.c" "status.c" "zlog.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_http_server
                       PRIV_REQUIRES radio_mux esp_timer esp_hw_support freertos heap log mbedtls esp_system
                                     openthread lwip board_cores reboot_gate netcfg logging timesync)
target_include_directories(${COMPONENT_LIB} PRIVATE "${spinel_dir}")
include(${CMAKE_CURRENT_LIST_DIR}/build_ziggurat.cmake)
