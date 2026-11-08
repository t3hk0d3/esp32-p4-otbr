// The board's built-in RCP: its descriptor and maintenance ops for the radio mux (radio_mux_builtin_t), and its
// status. builtin_rcp.c is chip-independent; the board's RCP is one file next to it (c6.c: the ESP32-C6 on the
// ESP32-P4 board). A board with another built-in RCP (an ESP32-H2) adds its own file and nothing else.
#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "rcp_sync.h"

#ifdef __cplusplus
extern "C" {
#endif

// What /device/status shows of the built-in RCP.
typedef struct {
    bool installed_known;     // the last probe reached the updater
    char installed[32];       // the RCP version the last probe read; "" = none (or unknown)
    bool rom_recovery_failed; // a ROM recovery failed and none worked since
    bool jumper;              // the maintenance jumper is fitted
} builtin_rcp_status_t;

// Describes the built-in RCP to the radio mux (radio_mux_set_builtin). Before radio_mux_start.
esp_err_t builtin_rcp_register(void);

// The status; any task.
void builtin_rcp_get_status(builtin_rcp_status_t *out);

/*
 * Push an RCP image to the built-in RCP on request (R14), as its channel's maintenance (radio_mux_maintain): the
 * mode instance on the channel lets go first (Thread: its sessions drain, OpenThread stops, the mode is STARTING; the
 * proxy refuses while a peer is connected; Zigbee pauses with its client connected), then rcp_sync_push_firmware, the
 * boot steps (the probe reads the pushed version), and the instance gets the channel back. Waits until that instance
 * runs again or says it does not (at most RADIO_MUX_SETTLE_MS). A push that worked whose mode then does not run is
 * RCP_SYNC_ERR_RESTART with `detail` "the image is installed, but <mode> did not start on it: <why>"; a failed push
 * returns its error (the previous firmware stays, the mode starts on it again). A base-only push (c6_installer.bin
 * through ESP-Hosted) returns ESP_OK and leaves the mode on "no rcp firmware". Refusals touch nothing:
 * ESP_ERR_INVALID_STATE while another maintenance or the boot steps run, OpenThread is being moved, or Thread refuses
 * (sniffer, starting, sessions open 45 s); RADIO_MUX_ERR_PEER_CONNECTED ("c6_busy: rcp-proxy on the C6 has a peer");
 * ESP_ERR_INVALID_ARG for a NULL source or read. `detail` (may be NULL) gets a short reason on failure. Blocks for
 * seconds to minutes; not from the radio mux owner task or the OpenThread task.
 */
esp_err_t builtin_rcp_push_firmware(const rcp_fw_source_t *source, char *detail, size_t cap);

/*
 * ROM recovery of the built-in RCP's base image on request (spec 5.10), with the discipline of
 * builtin_rcp_push_firmware: rcp_sync_rom_recover rewrites bootloader, partition table and updater, the boot steps'
 * probe reads the result, and the mode starts again (with the RCP that survived in its slot, else "no rcp firmware").
 * Returns the recovery's own result, whatever the mode did; ESP_ERR_NOT_SUPPORTED without the maintenance jumper
 * (nothing asked); the refusals of a push. Blocks for tens of seconds.
 */
esp_err_t builtin_rcp_rom_recover(char *detail, size_t cap);

#ifdef OTBR_TEST_BUILD
// Test builds: hold the C6 in reset instead of booting the RCP (persisted in NVS across reboots).
esp_err_t builtin_rcp_test_set_c6_hold(bool hold);
bool builtin_rcp_test_c6_hold(void);

typedef enum {
    BUILTIN_RCP_TEST_PROBE_REAL,  // run the real probe
    BUILTIN_RCP_TEST_PROBE_EMPTY, // pretend the C6 boot slot is empty
    BUILTIN_RCP_TEST_PROBE_FAIL,  // pretend the probe failed
} builtin_rcp_test_probe_t;

// Test builds: force the boot-time probe result (persisted in NVS across reboots).
esp_err_t builtin_rcp_test_set_probe_override(builtin_rcp_test_probe_t mode);
builtin_rcp_test_probe_t builtin_rcp_test_probe_override(void);
#endif

#ifdef __cplusplus
}
#endif
