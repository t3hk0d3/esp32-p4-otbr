#pragma once

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "openthread/link_raw.h"
#include "br_core_mode.h"
#include "radio_mux_orch_core.h"
#include "rcp_sync.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MODE_FSM_MIN_CHANNEL 11
#define MODE_FSM_MAX_CHANNEL 26
#define MODE_FSM_MAX_RECOVERIES 3
#define MODE_FSM_RECOVERY_WINDOW_US (10LL * 60 * 1000 * 1000)
/** A reset the radio mux could not post (ESP_ERR_TIMEOUT, ESP_ERR_NO_MEM: its queue full) is tried once more after
 * this long before it counts as failed. */
#define MODE_FSM_RESET_RETRY_MS 100
/** The built-in channel's id (RADIO_MUX_BUILTIN_ID; mode_fsm stays free of the mux). */
#define MODE_FSM_BUILTIN_ID "builtin-c6"
/** mode_fsm_move_otbr results besides ESP_OK and ESP_ERR_INVALID_STATE (busy). */
#define MODE_FSM_ERR_PROBE_FAILED ESP_ERR_INVALID_RESPONSE /**< the target's probe failed; nothing changed */
#define MODE_FSM_ERR_DRAIN_TIMEOUT ESP_ERR_TIMEOUT        /**< the OpenThread sessions did not end; nothing changed */
#define MODE_FSM_ERR_START_FAILED ESP_FAIL               /**< the start on the target failed; nothing stored */
/**
 * ot_start's result when OpenThread's init was abandoned (an error inside esp_openthread_init, a park from its
 * callbacks, a boot timeout): nothing can start OpenThread again until a reboot. mode_fsm_move_otbr returns it too.
 */
#define MODE_FSM_ERR_START_ABANDONED ESP_ERR_NOT_FINISHED
/** The reason of a drain timeout (the API's error code). */
#define MODE_FSM_DRAIN_TIMEOUT "drain_timeout"
/** The fault reason once OpenThread's init was abandoned: no start can run again until a reboot. */
#define MODE_FSM_FAULT_ABANDONED "an earlier OpenThread start was abandoned: reboot"

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
    /**
     * Hardware reset of the OTBR channel's RCP through the radio mux. ESP_ERR_TIMEOUT or ESP_ERR_NO_MEM: the request
     * could not be posted (the mux's queue stayed full); mode_fsm tries once more after MODE_FSM_RESET_RETRY_MS.
     */
    esp_err_t (*reset_rcp)(void);
    /** Block the calling task for `ms` milliseconds (the reset's retry delay). */
    void (*sleep_ms)(uint32_t ms);
    void (*reboot)(void);
    int64_t (*now_us)(void);
    /**
     * Probe the RCP of channel `id` through the radio mux (spinel reset, PROP_NCP_VERSION, the caps OpenThread needs)
     * while OpenThread may still run elsewhere. ESP_OK leaves the channel ready for ot_start; otherwise `err` says why.
     * `id` "" (a stop of OpenThread, mode_fsm_move_otbr): nothing to probe, it answers ESP_OK (the stop is accepted).
     */
    esp_err_t (*probe_channel)(const char *id, char *err, size_t cap);
    /**
     * Start OpenThread on channel `id` (probed before) and wait until it runs. A failure leaves nothing running;
     * MODE_FSM_ERR_START_ABANDONED when OpenThread's init was abandoned (only a reboot starts it again).
     */
    esp_err_t (*ot_start)(const char *id);
    /**
     * Stop OpenThread (Thread REST off, the open sessions drained, teardown). ESP_ERR_TIMEOUT when the sessions did not
     * drain: nothing was stopped. ESP_OK when nothing runs.
     */
    esp_err_t (*ot_stop)(void);
    /** The commit of a move: channel `to` in mode otbr and `from` in mode off (both stored modes), before `to` starts. */
    esp_err_t (*store_otbr)(const char *from, const char *to);
    /** The mode changed (to `mode`); Thread's state goes to the orchestrator's read API. Never blocks. May be NULL. */
    void (*mode_changed)(br_core_mode_t mode);
} mode_fsm_ops_t;

typedef struct {
    const mode_fsm_ops_t *ops;
    _Atomic br_core_mode_t mode;
    int64_t recovery_us[MODE_FSM_MAX_RECOVERIES]; /**< start times of the automatic recoveries in the window */
    size_t recovery_count;
    void (*sniffer_end_cb)(void); /**< see mode_fsm_set_sniffer_end_cb; NULL = none */
    bool thread_stopped_by_capture; /**< Thread/netif were stopped for a capture and not restored yet */
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

/** STARTING to BR_RUNNING once the border router is up; any other mode stays (a fault entered meanwhile wins). */
bool mode_fsm_set_started(mode_fsm_t *fsm);

/** Unrecoverable-here fault (init failure, dead radio): RADIO_FAULT from any mode, no recovery attempt. */
void mode_fsm_set_fault(mode_fsm_t *fsm);

/** ESP_ERR_INVALID_STATE unless BR_RUNNING, ESP_ERR_INVALID_ARG unless the channel is 11..26 and `cb` is not NULL. */
esp_err_t mode_fsm_enter_sniffer(mode_fsm_t *fsm, uint8_t channel, otLinkRawReceiveDone cb);

/** ESP_ERR_INVALID_STATE unless SNIFFER. A failed restore reboots the device. */
esp_err_t mode_fsm_leave_sniffer(mode_fsm_t *fsm);

/** Reason shown when OpenThread cannot start because NVS (its settings, the Thread credentials) is unusable. */
#define MODE_FSM_FAULT_STORAGE "storage unavailable"
/** Reason shown when the C6's RCP is rejected: OpenThread refused it, or it did not answer (reset, probe). */
#define MODE_FSM_FAULT_INCOMPATIBLE "rcp incompatible"

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

/**
 * The RCP has to be reset (OpenThread's hardware-reset hook). In STARTING it is only reset. Otherwise: end a
 * running capture (locally, `raw_abort`), RADIO_FAULT, then the recovery ladder (reset the C6; if that fails, stay in
 * RADIO_FAULT) unless 3 recoveries already started in the last 10 minutes. A firmware push is never part of the
 * ladder. Back to BR_RUNNING when the reset works; after a capture that happens in a deferred step (the RCP is only
 * usable again once the hook has returned): quiet the radio, restore Thread, BR_RUNNING (a failure reboots).
 */
void mode_fsm_on_rcp_failure(mode_fsm_t *fsm);

/**
 * The RCP reset of a failure round (`reset_rcp`): when it returns ESP_ERR_TIMEOUT or ESP_ERR_NO_MEM (the radio mux
 * could not take the request), it is tried once more after MODE_FSM_RESET_RETRY_MS (`sleep_ms`). Returns the last
 * result.
 */
esp_err_t mode_fsm_reset_rcp(mode_fsm_t *fsm);

/**
 * Result of a maintenance of the C6 (br_core's run_maintenance, after the work ran): `job_err` is the push or the ROM
 * recovery, `restart_err` the OpenThread start on the C6 that follows. A firmware push reports its own error first; a
 * push that worked followed by a failed restart is RCP_SYNC_ERR_RESTART (the image is installed, so a retry of the push
 * cannot help). A ROM recovery reports only its own result: the restart after it fails when the C6 has no RCP firmware
 * (the normal outcome of a recovery), and that state is in the status (RADIO_FAULT "no rcp firmware"), not an error of the recovery. A push of
 * c6_installer.bin through ESP-Hosted leaves the same state and counts as base-only too (`base_only` true for both).
 */
esp_err_t mode_fsm_radio_job_result(bool base_only, esp_err_t job_err, esp_err_t restart_err);

/**
 * The board mode at boot from the channel stored in otbr (NULL: none). The C6: true, the mode stays STARTING while
 * its boot steps run (the orchestrator then binds Thread to it, or tells it why it cannot). No channel or a dongle:
 * NO_RADIO (no fault) and false; a dongle starts when it is bound (mode_fsm_on_channel_back).
 */
bool mode_fsm_boot(mode_fsm_t *fsm, const char *otbr_id);

/**
 * Whether the failed RCP probe (`probe_err`) before a start of OpenThread on channel `id` (not a move's probe: that
 * refuses and changes nothing) is the C6's RCP rejected: the C6, and any failure but ESP_ERR_INVALID_STATE (the
 * channel went away, which br_radio handles as such). The fault is then MODE_FSM_FAULT_INCOMPATIBLE with rcp.compatible
 * false, as when OpenThread rejects it or it does not answer its reset (the Firmware page's install hint, the docs);
 * the probe's own text goes to the log. A dongle's probe failure keeps the probe's text. Pure.
 */
bool mode_fsm_probe_fault_incompatible(const char *id, esp_err_t probe_err);

/**
 * Whether an OpenThread (re)start failed in a way a PUT of mode otbr on its channel retries (br_core_retry_otbr): the
 * mode is RADIO_FAULT and OpenThread is not running (`ot_running`). Every path that tries a start (br_radio's start on
 * a bind, a move with its start back on the source) records it. Pure.
 */
bool mode_fsm_start_failed(br_core_mode_t mode, bool ot_running);

/**
 * Move OpenThread from channel `from` ("" for a start: no channel runs it) to `to` ("" for a stop of OpenThread on
 * `from`, which leaves otbr): a move is a rebind (design section 4.5). In order: probe `to` (refused with
 * MODE_FSM_ERR_PROBE_FAILED, the reason in `err`, nothing changed; a stop's probe only accepts); STARTING; stop
 * OpenThread on the source (MODE_FSM_ERR_DRAIN_TIMEOUT, `err` MODE_FSM_DRAIN_TIMEOUT, the mode before back, nothing
 * changed, never RADIO_FAULT); the commit, store_otbr(from, to) (a stop: NO_RADIO and done; a failure changed nothing:
 * OpenThread starts again on `from`, NO_RADIO without one, MODE_FSM_ERR_START_FAILED); then the start on `to`. A
 * failed start stays on `to`: RADIO_FAULT, MODE_FSM_ERR_START_FAILED, no start back on `from` (a PUT of mode otbr on
 * `to` retries, one on another channel moves it). A start that abandoned OpenThread's init: RADIO_FAULT, `err`
 * MODE_FSM_FAULT_ABANDONED, MODE_FSM_ERR_START_ABANDONED, no reboot. ESP_ERR_INVALID_STATE (nothing touched) in
 * STARTING and SNIFFER (a maintenance of the C6 keeps it STARTING). A successful start ends in BR_RUNNING through
 * mode_fsm_set_started (the border router start). Blocks for the whole move.
 */
esp_err_t mode_fsm_move_otbr(mode_fsm_t *fsm, const char *from, const char *to, char *err, size_t cap);

/**
 * The OTBR channel is gone (unplugged, failed, its mode changed): NO_RADIO at once (the guards refuse new Thread
 * sessions while the open ones drain), a capture ends (sniffer end callback), then OpenThread is torn down (ot_stop);
 * NO_RADIO again once it is (a fault entered meanwhile, e.g. a parked OpenThread task, does not stay), RADIO_FAULT when
 * the teardown did not finish.
 */
void mode_fsm_on_channel_gone(mode_fsm_t *fsm);

/**
 * The channel stored in otbr is bound (again): plug-in, the end of a maintenance, a mode change, the boot. STARTING,
 * probe, start; BR_RUNNING through the border router start. A failed probe or start ends in RADIO_FAULT.
 */
void mode_fsm_on_channel_back(mode_fsm_t *fsm, const char *id);

/** The board's view of Thread and the channel stored in otbr (design section 6). */
typedef struct {
    bool bound;                        /**< a channel is stored in otbr */
    radio_mux_chan_state_t chan_state; /**< that channel's lifecycle state */
    radio_mux_fault_kind_t kind;       /**< its fault kind (chan_state fault) */
    radio_mux_work_t work;             /**< its maintenance (chan_state reinit) */
    const char *chan_id;
    const char *chan_reason;           /**< its reason (fault: why) */
    /** Thread's own state: STARTING (starting, releasing), BR_RUNNING, SNIFFER or RADIO_FAULT while bound and started;
     *  NO_RADIO otherwise. */
    br_core_mode_t thread;
    /** RADIO_FAULT: the start's reason; NO_RADIO on a ready channel: why Thread is not bound to it (the fit). */
    const char *thread_reason;
    bool unplugged; /**< the channel went absent after Thread ran on it (not at boot) */
} mode_fsm_view_t;

/**
 * The board mode and its reason (into `reason`, cap `cap`) for a view, one row of the design's table per case:
 * no binding: NO_RADIO "no channel in otbr"; Thread starting: STARTING; running or sniffing on a ready channel:
 * BR_RUNNING or SNIFFER; in fault on a ready channel: RADIO_FAULT with the start's reason; the channel in a firmware
 * fault: RADIO_FAULT with its reason; in a link fault: NO_RADIO "<its reason> (<id>)"; in reinit: STARTING for the
 * built-in RCP's maintenance, NO_RADIO "waiting for <id>" for a dongle's detection; absent: NO_RADIO "waiting for <id>"
 * ("radio unplugged (<id>)" once it ran); ready but not bound (the fit): NO_RADIO with Thread's reason ("waiting for
 * <id>" when it has none). Pure.
 */
br_core_mode_t mode_fsm_mode_for(const mode_fsm_view_t *v, char *reason, size_t cap);

/**
 * Thread's answer to a reinit request (maintenance wants its channel): ESP_OK (it lets go once its sessions drained),
 * or ESP_ERR_INVALID_STATE in SNIFFER and STARTING. The drain's timeout refusal comes from the drain itself. Pure.
 */
esp_err_t mode_fsm_reinit_verdict(br_core_mode_t mode);

/**
 * REINIT (maintenance wants Thread's channel): refused with ESP_ERR_INVALID_STATE in SNIFFER and STARTING (nothing
 * touched, mode_fsm_reinit_verdict). Otherwise STARTING, then OpenThread stopped after its sessions drained (ot_stop):
 * a drain timeout puts the mode back and returns MODE_FSM_ERR_DRAIN_TIMEOUT (nothing stopped). ESP_OK: OpenThread is
 * stopped and the mode stays STARTING until the channel comes back (mode_fsm_on_channel_back) or is lost.
 */
esp_err_t mode_fsm_on_reinit(mode_fsm_t *fsm);

/** Whether the orchestrator's input `input_seq` is newer than the last one handled (`seen_seq`), the seq wrapping. */
bool mode_fsm_input_newer(uint32_t seen_seq, uint32_t input_seq);

#ifdef __cplusplus
}
#endif
