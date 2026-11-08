#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_netif.h"
#include "sdkconfig.h"
#include "openthread/link_raw.h"
#include "br_core_leader.h"
#include "br_core_mode.h"
#include "br_core_trel.h"
#include "rcp_sync.h"

#ifdef __cplusplus
extern "C" {
#endif

// Eventfds OpenThread uses: one each for the netif, the task queue and the border router, and one for TREL reception.
// Its radio is a mux channel, whose eventfd is the mux's (RADIO_MUX_EVENTFD_COUNT); ESP-IDF's SPI spinel interface and
// its eventfd are no longer used. app_main registers the eventfd VFS with these plus the mux's.
#if CONFIG_OPENTHREAD_RADIO_TREL
#define BR_CORE_EVENTFD_COUNT 4
#else
#define BR_CORE_EVENTFD_COUNT 3
#endif

// Room for a radio channel id (RADIO_MUX_ID_MAX).
#define BR_CORE_CHANNEL_ID_MAX 48

typedef struct {
    br_core_mode_t mode;
    bool storage_degraded;        /**< NVS did not initialise: OpenThread does not start (RADIO_FAULT "storage unavailable"), everything else runs */
    bool rcp_compatible;          /**< OpenThread accepted the C6's RCP at its last start on the C6 (a dongle never sets it) */
    bool rcp_compatible_known;    /**< OpenThread judged the C6's RCP this boot; false: not checked (Thread never ran there) */
    uint8_t channel;     /**< channel of the active dataset; 0 without one */
    bool ot_state_valid; /**< channel, has_dataset and pd_* were read from OpenThread; false when it is not up or its lock was not free within 100 ms (then they are unknown, not "none") */
    bool has_dataset;    /**< an active dataset is stored; only meaningful with ot_state_valid */
    int pd_state;        /**< otBorderRoutingDhcp6PdState; -1 while the border router is not initialised */
    char pd_prefix[48];  /**< delegated OMR prefix, "" without one */
    char fault_reason[64]; /**< why the mode is BR_MODE_RADIO_FAULT or BR_MODE_NO_RADIO ("radio unplugged (<id>)", "no channel in otbr", ...), "" otherwise */
    char otbr_channel[BR_CORE_CHANNEL_ID_MAX]; /**< the channel stored in mode otbr (present or not); "" for none (null) */
} br_core_status_t;

/**
 * Tell br_core how storage_init() ended (call it before br_core_start). A failure (e.g. NVS_NO_FREE_PAGES) degrades the
 * device instead of stopping it: OpenThread is not started (RADIO_FAULT with the reason "storage unavailable"), the HTTP
 * server, OTA and the device endpoints keep working. NVS is never erased for this: it holds the Thread credentials.
 */
void br_core_note_storage_init(esp_err_t storage_init_err);

/**
 * Register OpenThread as the radio mux's otbr consumer (one channel, spinel firmware, frames) and br_core_move_otbr as
 * its mover. Call it once before radio_mux_start. OpenThread then runs on whichever channel is stored in otbr and
 * present: it starts when that channel attaches and is torn down (NO_RADIO) when it goes.
 */
esp_err_t br_core_register_otbr(void);

/**
 * Move OpenThread from channel `from` (may be "" or absent: NO_RADIO) to `to` (radio_mux_set calls it for a PUT of mode
 * otbr while another channel has it, or while none has it: a start, `from` ""). The target's RCP is probed first,
 * through the mux, while OpenThread still runs on `from`; this call waits for that probe (up to 10 s). `to` "" is a stop
 * of OpenThread on `from` (a PUT of another mode on the otbr channel): nothing is probed, the call returns once the
 * stop is accepted, and br_core's radio task drains and stops OpenThread, then stores the requested mode
 * (radio_mux_commit_otbr); a drain timeout leaves everything as it was (br_core_otbr_refusal says so). Returns:
 * - ESP_OK: accepted (202). The rest runs on br_core's radio task: stop, start on `to`, store both modes once it runs.
 *   A drain timeout ("drain_timeout"), a failed start or a failed store (OpenThread started on `from` again; RADIO_FAULT
 *   only when that fails too) shows as `to`'s state fault with that reason, stored modes unchanged. A start on `to` that
 *   abandons OpenThread's init (nothing starts OpenThread again in this boot) shows the same way, and the board reboots
 *   1.5 s later onto the unchanged stored modes (Thread back on `from`, same network); fault_reason says so meanwhile.
 * - ESP_ERR_INVALID_RESPONSE: the probe failed (409 probe_failed); `reason` says why; nothing changed.
 * - ESP_ERR_INVALID_STATE: busy (another move, a C6 push or recovery, OpenThread starting or sniffing, or br_core's
 *   radio task stopping or starting OpenThread on its channel, e.g. after an unplug: refused at once); `reason`.
 * - ESP_ERR_TIMEOUT: the probe did not finish in time; the move is dropped after it.
 * `reason` (may be NULL) gets "" on ESP_OK.
 */
esp_err_t br_core_move_otbr(const char *from, const char *to, char *reason, size_t cap);

/** True from an accepted br_core_move_otbr until the move ended (device_api's long-operation state). */
bool br_core_move_running(void);

/**
 * The last refused stop of OpenThread (its sessions did not end within the drain): `id` gets the channel, which still
 * runs otbr, and `reason` why ("drain_timeout"); true then. False (both "") once a later move, start or stop was
 * accepted or a channel operation ran, or when none was refused. Any task.
 */
bool br_core_otbr_refusal(char *id, size_t id_cap, char *reason, size_t reason_cap);

/**
 * `done` (NULL: none) is called on br_core's radio task each time a move that br_core_move_otbr accepted (ESP_OK) has
 * ended, whatever its outcome, just before br_core_move_running turns false: device_api releases its long-operation
 * slot there. A move refused or given up before ESP_OK is not reported (the caller already knows). Must not block.
 */
void br_core_set_move_done_cb(void (*done)(void));

/**
 * Retry OpenThread's start on the attached OTBR channel after it failed (RADIO_FAULT from that start; otherwise it
 * waits for the channel's next attach). A PUT of mode otbr on that channel calls it. Does not wait; a no-op when the
 * last start did not fail, OpenThread runs, or no channel is attached.
 */
void br_core_retry_otbr(void);

/**
 * Start mDNS with `hostname` and the default service instance name `instance` (the names come from netnames). Idempotent;
 * the names are kept for the retry br_core_start makes when this failed. Needs esp_netif_init() and the default event
 * loop. app_main calls it before anything slow so that the HTTP server can start right after.
 */
esp_err_t br_core_mdns_start(const char *hostname, const char *instance);

/**
 * Boot the radio side (spec 5.3 steps 3 to 5): start mDNS (a retry, with the names
 * br_core_mdns_start was given). With no channel stored in otbr, or a dongle there, the mode is NO_RADIO (no fault) and
 * OpenThread starts when that dongle attaches. With the C6 (the default), inside a C6 job of the radio mux: probe the
 * installed RCP version (best effort, nothing is ever pushed at boot) and reset the C6 into its RCP; at the job's end the
 * mux attaches the C6, and OpenThread starts on it through the mux (the RCP probed through the channel first) on core 0;
 * the border router starts once `backbone` has an IPv6 address. The C6's boot steps (the probe, the jumper's detection,
 * the automatic ROM recovery) run at every boot: with another channel in otbr, or none, they only update the C6's status
 * and listing, and OpenThread starts on its own channel meanwhile. The border router start
 * enables DHCPv6-PD and starts Thread when an active dataset is stored; otherwise Thread stays idle.
 *
 * Blocks until OpenThread is initialised, the radio fault mode was entered or the mode is NO_RADIO, and returns ESP_OK in
 * all cases (a radio fault is reported through br_core_get_mode: fault_reason "no rcp firmware" for an empty C6 boot
 * slot, "rcp incompatible" when OpenThread rejects the RCP). The border router start itself may come later.
 * Call it from a task pinned to core 0 after esp_netif_init(), the default event loop and the storage mounts.
 *
 * @param backbone the Ethernet netif (the backbone interface); must not be NULL
 * A failing mDNS start is logged and does not stop the radio start (the board is then reachable by address only).
 * @return ESP_ERR_INVALID_ARG for NULL, ESP_ERR_INVALID_STATE if called twice, ESP_ERR_NO_MEM
 */
esp_err_t br_core_start(esp_netif_t *backbone);

br_core_mode_t br_core_get_mode(void);

/**
 * Set the hook that owns OpenThread's CLI (set it before br_core_start). It is called with true after every OpenThread
 * initialisation, on the bring-up task with the OpenThread lock held (install the CLI there: its task queue exists
 * from then on), and with false before every esp_openthread_deinit, without the lock (stop posting to the CLI).
 */
void br_core_set_ot_cli_hook(void (*hook)(bool running));

/** True only in BR_MODE_BR_RUNNING: the REST guard allows Thread operations. */
bool br_core_thread_ops_allowed(void);

/** Snapshot of the mode and, when OpenThread is up, of the dataset and DHCPv6-PD state (ot_state_valid says whether they were read). Never blocks long. */
void br_core_get_status(br_core_status_t *out);

/** Like br_core_get_status but never takes the OpenThread lock: ot_state_valid is false, channel and pd_* are not read. */
void br_core_get_status_brief(br_core_status_t *out);

/**
 * Set whether TREL runs. Before br_core_start it only sets the value the start applies; afterwards it is applied to
 * OpenThread at once (otTrelSetEnabled; TREL then runs whenever Thread is up). Storing the setting is the caller's job
 * (netcfg_set_trel). ESP_ERR_NOT_SUPPORTED in a build without TREL; ESP_ERR_INVALID_STATE when OpenThread is not up (the
 * value is applied when it starts); ESP_ERR_TIMEOUT when the OpenThread lock was not free
 * within 1 s (the value is still applied at the next OpenThread start).
 */
esp_err_t br_core_set_trel(bool enabled);

/** The TREL setting (br_core_set_trel); never takes a lock. */
bool br_core_trel_wanted(void);

/** TREL setting and, when OpenThread is up and its lock free within 100 ms, its state, counters and peers. */
void br_core_get_trel(br_core_trel_t *out);

/**
 * Fill `out` (state_valid, active, counters, both peer counts) and, unless `list` is NULL, the sorted peer list from one
 * walk of the TREL peer table. The caller holds the OpenThread lock and has checked that OpenThread is up. Leaves both
 * untouched in a build without TREL. Shared by br_core_get_trel and the metrics collector (counts only), so both
 * report alike.
 */
void br_core_trel_read_locked(otInstance *instance, br_core_trel_state_t *out, br_core_trel_list_t *list);

/**
 * OpenThread session: while one is open a radio push or ROM recovery does not tear OpenThread down (its lock and
 * instance stay valid), so the holder may take and release the OpenThread lock as often as it needs, including around
 * long waits for OpenThread callbacks. Returns false while OpenThread is not running or a push is about to stop it:
 * then nothing is open and the caller must not touch OpenThread. Does not take the OpenThread lock. Sessions nest.
 * Every task other than the OpenThread task opens one around its OpenThread access. Close it with
 * br_core_ot_session_end. A push, a move or a deliberate stop (the OTBR channel's mode changed) waits up to 45 s for the
 * open sessions to close and is refused (nothing stopped) if they do not. When the OTBR channel is lost (unplugged,
 * failed) the mode is NO_RADIO at once and the teardown waits for every open session however long it takes (logged
 * every 20 s): OpenThread's lock is never deleted under a session. Its radio calls are answered at once meanwhile (as a
 * refusal or a no-op), so it ends quickly. A holder that waits for an OpenThread callback should give up once the Thread
 * REST flag (esp_br_web_openthread_ready) is off.
 */
bool br_core_ot_session_begin(void);

/** Closes a session opened by br_core_ot_session_begin (only after it returned true). */
void br_core_ot_session_end(void);

/**
 * BR_RUNNING to SNIFFER (spec 5.4): Thread stops, the OpenThread netif goes down (Ethernet stays up), the raw link
 * receives promiscuously on `channel`. `receive_cb` runs in the OpenThread task for every frame.
 * ESP_ERR_INVALID_STATE unless BR_RUNNING (also while a firmware push has taken OpenThread over), ESP_ERR_INVALID_ARG unless 11..26 and `receive_cb` is not NULL (nothing changes then). If the raw link cannot be enabled,
 * Thread is restored and the error returned.
 */
esp_err_t br_core_enter_sniffer(uint8_t channel, otLinkRawReceiveDone receive_cb);

/**
 * Register (NULL clears) the callback that tells the capture owner a capture ended on its own (R17): it runs once
 * whenever the mode leaves SNIFFER other than through br_core_leave_sniffer, i.e. an RCP failure while sniffing
 * (RADIO_FAULT, then the recovery ladder) or a radio fault (fault_and_park). At the call the mode is already RADIO_FAULT. It runs on the OpenThread task with the OpenThread lock
 * held (or on the task that raised the fault): it must only set flags and wake waiters, never block, log per
 * frame, or call back into br_core. After it ran no more frames reach the receive callback. Register it before
 * br_core_enter_sniffer. A capture ended by br_core_leave_sniffer does not trigger it.
 */
void br_core_set_sniffer_end_cb(void (*cb)(void));

/**
 * Register (NULL clears) the hook that runs once the border router is initialised, at boot, before Thread starts. It
 * runs on the bring-up task with the OpenThread lock held and owns what follows border router init: the DHCPv6-PD
 * client stays off unless it turns it on (the routing component registers routing_on_border_router_up). Register it
 * before br_core_start.
 */
void br_core_set_border_router_up_cb(void (*cb)(otInstance *instance));

/**
 * SNIFFER to BR_RUNNING: raw link off, dataset checked, netif up, Thread restarted as before. If that fails the
 * device reboots (sniffer mode is never persisted). ESP_ERR_INVALID_STATE unless SNIFFER.
 */
esp_err_t br_core_leave_sniffer(void);

/**
 * Liveness probe for an idle capture: while SNIFFER, one synchronous request to the RCP (OpenThread sends none while
 * it only receives, so a dead RCP would go unnoticed until the capture is stopped). Posts one request to the
 * OpenThread task and returns at once (never more than one is pending). If the RCP does not answer, the normal RCP
 * failure path runs there (RADIO_FAULT, the sniffer end callback, recovery). A no-op in any other mode. Call it every
 * second or so.
 */
void br_core_sniffer_keepalive(void);

/**
 * Push an RCP image to the C6 on request (R14), as the C6 channel's maintenance (radio_mux_maintain): the C6's instance
 * lets go first, then rcp_sync_push_firmware, the C6's boot steps (the probe reads the pushed version), and the
 * instance gets the C6 back. With Thread on the C6: its sessions drain, OpenThread stops (the mode is STARTING and the
 * Thread REST API off meanwhile), and this waits until OpenThread runs on the C6 again; works from BR_RUNNING and
 * RADIO_FAULT (also "no rcp firmware"), leaves RADIO_FAULT on success, and a failed push restarts OpenThread with the
 * previous firmware and returns the push error. Blocks for seconds to minutes on the caller's (management) task.
 * Refusals touch nothing: ESP_ERR_INVALID_STATE from the OpenThread task, in SNIFFER and STARTING (Thread on the C6),
 * while the boot steps run ("the radio is still booting"), another push or recovery runs, OpenThread is being moved,
 * or OpenThread sessions (br_core_ot_session_begin) stay open for 45 s ("OpenThread is busy, try again");
 * RADIO_MUX_ERR_PEER_CONNECTED while rcp-proxy on the C6 has a connected peer. ESP_ERR_INVALID_ARG for a NULL source
 * or read. `err_detail` (may be NULL) gets a short reason on failure. After an abandoned OpenThread start a push that
 * worked ends in a reboot ~1.5 s later (the return value is ESP_OK, the mode RADIO_FAULT until then). When the C6 is
 * not the channel stored in otbr (br_core_c6_job_touches_openthread false) OpenThread and the board mode are left
 * alone.
 */
esp_err_t br_core_push_radio_firmware(const rcp_fw_source_t *source, char *err_detail, size_t err_cap);

/**
 * Whether a maintenance of the C6 (br_core_push_radio_firmware, br_core_rom_recover) stops and restarts OpenThread:
 * the C6 is the channel stored in otbr. Otherwise it leaves OpenThread and the board mode alone, and the mode's
 * refusals (SNIFFER, STARTING) do not apply to it: the mode is another channel's. A snapshot.
 */
bool br_core_c6_job_touches_openthread(void);

/**
 * ROM recovery of the C6 base image on request (spec 5.10), as the C6 channel's maintenance with the discipline of
 * br_core_push_radio_firmware: rcp_sync_rom_recover rewrites bootloader, partition table and updater (no RCP firmware),
 * the boot steps' probe reads the result, and with Thread on the C6 OpenThread starts again: with the RCP that
 * survived in its slot the BR runs again, otherwise the mode is RADIO_FAULT "no rcp firmware" until firmware is
 * uploaded. ESP_ERR_NOT_SUPPORTED without the jumper (nothing is touched); the refusals of a push. A failed recovery
 * returns its error (the boot steps decide the mode). `err_detail` (may be NULL) gets a short reason on failure.
 * Blocks for tens of seconds.
 */
esp_err_t br_core_rom_recover(char *err_detail, size_t err_cap);

/** Whether br_core_set_leader_adjustment applied the value to OpenThread. */
typedef enum {
    BR_CORE_LEADER_APPLIED,   /**< OpenThread uses the value now */
    BR_CORE_LEADER_NOT_READY, /**< OpenThread is not usable (RADIO_FAULT, not started, a push) or no session opened */
    BR_CORE_LEADER_BUSY,      /**< the 1 s wait for the OpenThread lock ran out */
} br_core_leader_apply_t;

/**
 * Store the leader weight adjustment (NVS namespace `thread`, key `leader_adj`; written only when it changes, `net reset`
 * keeps it) and apply it at once whenever OpenThread is ready (any mode but RADIO_FAULT: STARTING, BR_RUNNING, SNIFFER):
 * OpenThread's device properties recompute the local leader weight. When OpenThread is not ready or its lock is not free
 * within 1 s, the value is stored and applied at the next border router start or the next br_core_leader_weight read,
 * whichever comes first. Safe to call from several tasks: a mutex serialises the compare, the NVS write and the store.
 * @param adj BR_CORE_LEADER_ADJ_MIN..BR_CORE_LEADER_ADJ_MAX
 * @param result set on ESP_OK to BR_CORE_LEADER_APPLIED, BR_CORE_LEADER_NOT_READY (OpenThread not usable) or
 *        BR_CORE_LEADER_BUSY (the lock wait ran out); NOT_READY on every error
 * @return ESP_ERR_INVALID_ARG out of range or NULL `result` (nothing stored), ESP_ERR_INVALID_STATE when storage is
 *         degraded or br_core_note_storage_init was not called (nothing changes), the NVS error when the write fails
 */
esp_err_t br_core_set_leader_adjustment(int8_t adj, br_core_leader_apply_t *result);

/** The stored leader weight adjustment: 0 when nothing is stored or storage is degraded. Never takes a lock. */
int8_t br_core_leader_adjustment(void);

/**
 * This board's leader weight (otThreadGetLocalLeaderWeight: 64 + border router + power supply + the adjustment), with
 * the bounded lock wait of br_core_get_status. A stored adjustment that OpenThread does not use yet (a save that could
 * not take the lock) is applied first, so the weight always matches br_core_leader_adjustment.
 * @return ESP_ERR_INVALID_STATE when OpenThread is not running (RADIO_FAULT, not started, a push), ESP_ERR_TIMEOUT when
 *         the lock was not free in time
 */
esp_err_t br_core_leader_weight(uint8_t *out);

/**
 * Take over as leader on explicit user request (never automatic): under the OpenThread lock (1 s wait) read the role,
 * router eligibility and both weights, run br_core_leader_check, then otThreadBecomeLeader. OpenThread starts a new
 * partition with this board as leader; the old leader reattaches.
 * @param refusal set to BR_CORE_LEADER_OK, or the reason with ESP_ERR_INVALID_STATE (not BR_RUNNING: NOT_RUNNING;
 *        OpenThread's own refusal: OT_ERROR_NOT_CAPABLE as WEIGHT_NOT_HIGHER, OT_ERROR_INVALID_STATE as NOT_ATTACHED)
 * @param weight may be NULL; on ESP_OK set to this board's local leader weight, read under the same lock before the
 *        take-over
 * @return ESP_OK started, ESP_ERR_INVALID_STATE refused, ESP_ERR_NOT_FINISHED when OpenThread is being stopped or
 *         restarted (radio push: the mode is still BR_RUNNING but OpenThread no longer takes calls), ESP_ERR_TIMEOUT when
 *         the lock was not free within 1 s, ESP_ERR_INVALID_ARG for NULL `refusal`, ESP_FAIL for any other OpenThread error
 */
esp_err_t br_core_become_leader(br_core_leader_refusal_t *refusal, uint8_t *weight);

/**
 * Start the app validity timer: once the image is pending-verify, the app has run 60 s and the HTTP server
 * is listening (esp_br_web_is_server_started), mark it valid. Radio state, Ethernet link, sync result and
 * OpenThread state are deliberately not criteria. Safe to call early: the 60 s count from boot.
 */
void validity_start(void);

#ifdef OTBR_TEST_BUILD
/** Test builds: the next OpenThread start on channel `id` ends as an abandoned one (MODE_FSM_ERR_START_ABANDONED) at
 *  once, as an init stuck past its wait would, and nothing starts OpenThread again until a reboot. NULL or "" clears
 *  it. Not persisted. */
void br_core_test_set_start_abandon(const char *id);
#endif

#ifdef __cplusplus
}
#endif
