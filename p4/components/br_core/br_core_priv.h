// Shared between br_core.c (the OpenThread state machine's operations, the boot, the C6 jobs) and br_core_radio.c
// (OpenThread on a radio mux channel). Not part of br_core's API.
#pragma once

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mode_fsm.h"
#include "radio_mux.h"

#define BOOT_WAIT_MS 180000
// Longest a stop waits for the open OpenThread sessions. The web server's callback waits end within ~0.25 s of the
// REST flag going off, so this only bounds a session held by slow HTTP I/O or a stuck OpenThread call.
#define SESSION_DRAIN_MS 45000
#define BRINGUP_WAIT_MS 60000

typedef struct {
    StaticSemaphore_t mutex_storage;
    SemaphoreHandle_t mutex;     // guards the fields below
    SemaphoreHandle_t boot_done; // given once OpenThread is initialised or the fault mode was entered
    SemaphoreHandle_t loop_done; // given by the OpenThread task when it left the main loop for a firmware push
    SemaphoreHandle_t bringup_done; // given when the border router task ended
    SemaphoreHandle_t sessions_done; // given when the last open OpenThread session closed
    esp_netif_t *backbone;
    esp_netif_t *ot_netif;
    TaskHandle_t ot_task;
    char fault_reason[64];
    char rcp_installed[32];    // version from the last probe, "" = none
    bool rcp_installed_known;  // the last probe reached the updater
    bool rcp_compatible;       // OpenThread accepted the RCP at the last start
    bool rom_recovery_failed;  // a ROM recovery failed and no later one worked
    bool ot_ready;             // OpenThread runs and takes calls; cleared before a push tears it down
    bool ot_initialised;       // esp_openthread_init succeeded and not yet torn down
    bool init_abandoned;       // OpenThread's start was abandoned half-way (its SPI driver stays registered): only a reboot restarts it
    bool address_seen;
    bool bringup_started;
    bool border_router_up;
    bool loop_running;
    bool push_pending;         // a firmware push asked the OpenThread task to leave its main loop
    uint32_t ot_sessions;      // open br_core_ot_session_begin sessions; teardown waits for 0
    atomic_bool push_busy;     // a firmware push is running (one long operation at a time)
    bool started;
    bool mdns_started;
    char mdns_hostname[64]; // as given to br_core_mdns_start, for the retry in br_core_start
    char mdns_instance[64];
} core_state_t;

extern core_state_t g_core;
extern bool g_storage_degraded; // storage_init failed: OpenThread does not start
extern mode_fsm_t g_fsm;        // the only owner of the board mode

/* ---- br_core.c ---- */

void core_set_reason(const char *reason);
// Radio fault: the Thread REST API off, the reason, RADIO_FAULT; br_core_start's wait ends.
void core_enter_fault(const char *reason);
// Stops everything OpenThread owns (main loop, border router, netif, esp_openthread_deinit). Not from the OT task.
void core_teardown_openthread(void);
// After ot_ready was cleared: false when sessions stay open longer than `wait_ms`.
bool core_wait_sessions_closed(uint32_t wait_ms);
// Reboots in 1.5 s (the caller reports first).
void core_schedule_reboot(void);
bool core_ot_ready_now(void);
esp_err_t core_start_ot_task(void);
bool core_backbone_has_ip6(void);
// The C6's boot steps inside a C6 job (see br_core.c).
esp_err_t core_c6_boot_steps(bool affects_mode);

/* ---- br_core_radio.c ---- */

// The channel OpenThread runs on (NULL when stopped), and whether it went (detached or GONE).
radio_mux_chan_t *core_radio_chan(void);
bool core_radio_gone(void);
// esp_openthread_mux_set for that channel (before every esp_openthread_init).
void core_radio_mux_set(void);
// Wakes br_radio: it reconciles OpenThread with the attached channel.
void core_radio_notify(void);
// The boot of the OTBR channel (from br_core_start).
void core_radio_boot(void);
// OpenThread stopped (no session drain) and its channel forgotten.
void core_radio_stop_openthread(void);
// OpenThread on the C6 again after a C6 job (probe and start).
esp_err_t core_radio_restart_on_c6(void);
// The mode_fsm channel operations.
esp_err_t core_radio_probe_channel(const char *id, char *err, size_t cap);
esp_err_t core_radio_ot_start(const char *id);
esp_err_t core_radio_ot_stop(void);
esp_err_t core_radio_store_otbr(const char *from, const char *to);
