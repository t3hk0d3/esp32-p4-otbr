// The board's built-in RCP: its descriptor and maintenance ops for the radio mux (radio_mux_builtin_t), and its
// status. builtin_rcp.c is chip-independent; the board's RCP is one file next to it (c6.c: the ESP32-C6 on the
// ESP32-P4 board). A board with another built-in RCP (an ESP32-H2) adds its own file and nothing else.
#pragma once

#include <stdbool.h>

#include "esp_err.h"

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
