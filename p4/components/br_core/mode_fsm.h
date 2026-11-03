#pragma once

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "openthread/link_raw.h"
#include "br_core_mode.h"
#include "rcp_sync.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MODE_FSM_MIN_CHANNEL 11
#define MODE_FSM_MAX_CHANNEL 26
#define MODE_FSM_MAX_RECOVERIES 3
#define MODE_FSM_RECOVERY_WINDOW_US (10LL * 60 * 1000 * 1000)

/** Everything the FSM does to the outside world; br_core.c provides the real ones, host tests fakes. */
typedef struct {
    esp_err_t (*thread_stop)(void);
    esp_err_t (*netif_down)(void);
    /** Raw link on, then channel, promiscuous mode and receive through the raw link. Cleans up after itself on failure. */
    esp_err_t (*raw_enable)(uint8_t channel, otLinkRawReceiveDone cb);
    esp_err_t (*raw_disable)(void);
    /**
     * Switch the raw link off locally, without a single request to the RCP (it is dead or being reset: any request
     * would time out and start another failure round). The radio state is cleaned up later by `quiet_radio`.
     */
    esp_err_t (*raw_abort)(void);
    /** After an RCP reset that ended a capture: promiscuous mode off and the radio asleep (requests to a live RCP). */
    esp_err_t (*quiet_radio)(void);
    /** Run `fn(ctx)` once on the OpenThread task, after the current failure hook has returned. */
    esp_err_t (*defer)(void (*fn)(void *ctx), void *ctx);
    /** Make sure the dataset channel is what it was before the capture. Never calls otLinkSetChannel. */
    esp_err_t (*restore_channel)(void);
    esp_err_t (*netif_up)(void);
    esp_err_t (*thread_start)(void);
    /** Hardware reset of the RCP (GPIO54). */
    esp_err_t (*reset_rcp)(void);
    /**
     * Stop OpenThread, push the firmware, hardware-reset the C6, start OpenThread again. Returns the push error
     * (or the restart error when the push worked). *restarted tells whether OpenThread runs again, whatever the
     * push result: a failed push restarts it with the previous firmware.
     */
    esp_err_t (*push_firmware)(const rcp_fw_source_t *source, char *err_detail, size_t err_cap, bool *restarted);
    /** As push_firmware, but the job is a ROM recovery of the C6 base image over the maintenance jumper (no image). */
    esp_err_t (*rom_recover)(char *err_detail, size_t err_cap, bool *restarted);
    void (*reboot)(void);
    int64_t (*now_us)(void);
} mode_fsm_ops_t;

typedef struct {
    const mode_fsm_ops_t *ops;
    _Atomic br_core_mode_t mode;
    int64_t recovery_us[MODE_FSM_MAX_RECOVERIES]; /**< start times of the automatic recoveries in the window */
    size_t recovery_count;
    void (*sniffer_end_cb)(void); /**< see mode_fsm_set_sniffer_end_cb; NULL = none */
    bool thread_stopped_by_capture; /**< Thread/netif were stopped for a capture and not restored yet */
    bool radio_job; /**< a push or ROM recovery began (mode_fsm_begin_radio_job) and has not ended */
    br_core_mode_t radio_job_prev; /**< the mode before it, put back by mode_fsm_cancel_radio_job */
} mode_fsm_t;

/** Mode STARTING. `ops` must outlive the FSM. */
void mode_fsm_init(mode_fsm_t *fsm, const mode_fsm_ops_t *ops);

/**
 * Register (NULL clears) the callback that tells the capture owner it ended without its own leave call (R17): it
 * runs once whenever the mode leaves SNIFFER through mode_fsm_set_fault or mode_fsm_on_rcp_failure, after the mode
 * left SNIFFER (RADIO_FAULT). It does not run for mode_fsm_leave_sniffer. Call it before the first capture.
 */
void mode_fsm_set_sniffer_end_cb(mode_fsm_t *fsm, void (*cb)(void));

/** Lock-free, callable from any task. */
br_core_mode_t mode_fsm_get_mode(const mode_fsm_t *fsm);

/** True only in BR_MODE_BR_RUNNING. */
bool mode_fsm_thread_ops_allowed(const mode_fsm_t *fsm);

/** STARTING to BR_RUNNING once the border router is up; any other mode stays (a fault entered meanwhile wins). No
 *  change while a push or ROM recovery runs (its STARTING ends with the job). */
bool mode_fsm_set_started(mode_fsm_t *fsm);

/** Unrecoverable-here fault (init failure, dead radio): RADIO_FAULT from any mode, no recovery attempt. */
void mode_fsm_set_fault(mode_fsm_t *fsm);

/** ESP_ERR_INVALID_STATE unless BR_RUNNING, ESP_ERR_INVALID_ARG unless the channel is 11..26 and `cb` is not NULL. */
esp_err_t mode_fsm_enter_sniffer(mode_fsm_t *fsm, uint8_t channel, otLinkRawReceiveDone cb);

/** ESP_ERR_INVALID_STATE unless SNIFFER. A failed restore reboots the device. */
esp_err_t mode_fsm_leave_sniffer(mode_fsm_t *fsm);

/**
 * Whether a firmware push may start: ESP_ERR_INVALID_STATE in STARTING and SNIFFER, and from the OpenThread task
 * (it cannot stop its own main loop). Pure; br_core calls it before it touches any state.
 */
esp_err_t mode_fsm_push_allowed(br_core_mode_t mode, bool caller_is_openthread_task);

/** Reason shown when OpenThread cannot start because NVS (its settings, the Thread credentials) is unusable. */
#define MODE_FSM_FAULT_STORAGE "storage unavailable"

/**
 * Whether the storage init result leaves the device degraded: any error (NVS full or corrupt) does. NVS is never erased
 * for it, because it holds the Thread credentials; the HTTP server, OTA and the device endpoints keep working, OpenThread does not start.
 */
bool mode_fsm_storage_degraded(esp_err_t storage_init_err);

/** Whether OpenThread may be started: not while the storage is degraded. */
bool mode_fsm_radio_start_allowed(bool storage_degraded);

/** What to do with the task that OpenThread's radio failure hook (RecoverFromRcpFailure) runs in. */
typedef enum {
    MODE_FSM_FAULT_PARK_TASK, /**< the OpenThread task: delete it (the main loop is gone with it); the rest keeps running */
    MODE_FSM_FAULT_REBOOT,    /**< any other task (httpd, thread_rest, pcap_stream, bring-up): never delete it, reboot */
} mode_fsm_fault_action_t;

/**
 * OpenThread calls its failure hooks synchronously in whichever task used the radio, and aborts the process when
 * the hook returns. Deleting the OpenThread task is harmless; deleting a task that serves something else (the HTTP
 * server) would leave the board without it until a power cycle, so there the device reboots. Pure.
 */
mode_fsm_fault_action_t mode_fsm_fault_action(bool caller_is_openthread_task);

/** Final state of the Thread REST flag after a push, whatever its outcome: on only in BR_RUNNING with OpenThread up. */
bool mode_fsm_rest_ready_after_push(br_core_mode_t mode, bool openthread_ready);

/**
 * Start a push or ROM recovery: ESP_ERR_INVALID_STATE in STARTING and SNIFFER (nothing changes). Otherwise the mode
 * becomes STARTING (the REST guard refuses Thread access, the border router start does not complete, an RCP failure
 * only resets the C6) until the job ends. Called under the OpenThread lock while OpenThread runs, so that the check
 * and the change cannot be separated by a mode change.
 */
esp_err_t mode_fsm_begin_radio_job(mode_fsm_t *fsm);

/** A begun job that will not run (nothing was stopped): the mode before mode_fsm_begin_radio_job comes back. */
void mode_fsm_cancel_radio_job(mode_fsm_t *fsm);

/**
 * Push an RCP image on request. Begins the job (mode_fsm_begin_radio_job) unless the caller already did:
 * ESP_ERR_INVALID_STATE in STARTING and SNIFFER (nothing is touched). The mode is STARTING while the push runs and
 * BR_RUNNING afterwards when OpenThread runs again (also after a failed push, which then returns its error),
 * RADIO_FAULT when it does not. Works from RADIO_FAULT.
 */
esp_err_t mode_fsm_push_firmware(mode_fsm_t *fsm, const rcp_fw_source_t *source, char *err_detail, size_t err_cap);

/** ROM recovery on request (spec 5.10): the same mode discipline as mode_fsm_push_firmware, with the rom_recover op as the job. */
esp_err_t mode_fsm_rom_recover(mode_fsm_t *fsm, char *err_detail, size_t err_cap);

/**
 * The RCP has to be reset (OpenThread's hardware-reset hook). In STARTING it is only reset. Otherwise: end a
 * running capture (locally, `raw_abort`), RADIO_FAULT, then the recovery ladder (reset the C6; if that fails, stay in
 * RADIO_FAULT) unless 3 recoveries already started in the last 10 minutes. A firmware push is never part of the
 * ladder. Back to BR_RUNNING when the reset works; after a capture that happens in a deferred step (the RCP is only
 * usable again once the hook has returned): quiet the radio, restore Thread, BR_RUNNING (a failure reboots).
 */
void mode_fsm_on_rcp_failure(mode_fsm_t *fsm);

/**
 * What a failed ROM recovery in the boot probe means: RADIO_FAULT "rom recovery failed" only when the updater was
 * silent before the attempt. When it was reachable (the trigger was a base version mismatch) an RCP may be installed
 * and the boot goes on with it; the failure is only recorded in the status. False when no recovery failed.
 */
bool mode_fsm_rom_failure_is_fault(const rcp_sync_probe_result_t *probe);

/**
 * The sticky "a ROM recovery failed" flag of the status: unchanged without an attempt, set by a failed attempt, cleared
 * by a successful one. Applies to automatic (boot probe) and manual recoveries alike.
 */
bool mode_fsm_next_recovery_failed(bool previous, bool attempted, bool succeeded);

/**
 * Result of a C6 job (the part of op_radio_job after the job ran): `job_err` is the push or the ROM recovery, `restart_err`
 * the OpenThread restart that follows. A firmware push reports its own error first; a push that worked followed by a
 * failed restart is RCP_SYNC_ERR_RESTART (the image is installed, so a retry of the push cannot help). A ROM recovery
 * reports only its own result: the restart after it fails when the C6 has no RCP firmware (the normal outcome of a
 * recovery), and that state is in the status (RADIO_FAULT "no rcp firmware"), not an error of the recovery. A push of
 * c6_installer.bin through ESP-Hosted leaves the same state and counts as base-only too (`base_only` true for both).
 */
esp_err_t mode_fsm_radio_job_result(bool base_only, esp_err_t job_err, esp_err_t restart_err);

/**
 * What the status shows as the installed RCP version after a push whose version was read back from the C6 (`probe_ok`) or
 * not: the read-back version, or, when it could not be read, unknown (`*known` false, `out` empty) — never the version
 * from before the push.
 */
void mode_fsm_installed_after_push(bool probe_ok, const char *probed_version, bool *known, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
