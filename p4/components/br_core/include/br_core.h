#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_netif.h"
#include "openthread/link_raw.h"
#include "br_core_mode.h"
#include "rcp_sync.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    br_core_mode_t mode;
    bool storage_degraded;        /**< NVS did not initialise: OpenThread does not start (RADIO_FAULT "storage unavailable"), everything else runs */
    char rcp_installed[32];       /**< version of the RCP firmware installed in the C6; "" = none (or unknown) */
    bool rcp_installed_known;     /**< false when the boot-time probe could not read the version */
    bool rcp_compatible;          /**< OpenThread accepted the RCP at the last start */
    bool rom_recovery_failed;     /**< a ROM recovery failed and none worked since (the boot went on with the installed RCP, or RADIO_FAULT) */
    uint8_t channel;     /**< channel of the active dataset; 0 without one */
    bool ot_state_valid; /**< channel, has_dataset and pd_* were read from OpenThread; false when it is not up or its lock was not free within 100 ms (then they are unknown, not "none") */
    bool has_dataset;    /**< an active dataset is stored; only meaningful with ot_state_valid */
    int pd_state;        /**< otBorderRoutingDhcp6PdState; -1 while the border router is not initialised */
    char pd_prefix[48];  /**< delegated OMR prefix, "" without one */
    char fault_reason[64]; /**< why the mode is BR_MODE_RADIO_FAULT, "" otherwise */
} br_core_status_t;

/**
 * Tell br_core how storage_init() ended (call it before br_core_start). A failure (e.g. NVS_NO_FREE_PAGES) degrades the
 * device instead of stopping it: OpenThread is not started (RADIO_FAULT with the reason "storage unavailable"), the HTTP
 * server, OTA and the device endpoints keep working. NVS is never erased for this: it holds the Thread credentials.
 */
void br_core_note_storage_init(esp_err_t storage_init_err);

/**
 * Start mDNS with the hostname esp-otbr-<last 3 MAC bytes, lowercase hex>. Idempotent. Needs esp_netif_init() and
 * the default event loop. app_main calls it before anything slow so that the HTTP server can start right after.
 */
esp_err_t br_core_mdns_start(void);

/**
 * Boot the radio side (spec 5.3 steps 3 to 5): start mDNS with the hostname esp-otbr-<last 3 MAC bytes,
 * lowercase hex>, probe the installed RCP version (best effort, nothing is ever pushed at boot), reset the C6 into
 * its RCP, start OpenThread with RADIO_MODE_SPI_RCP on core 0 and
 * arrange for the border router to start once `backbone` has an IPv6 address. The border router start
 * enables DHCPv6-PD and starts Thread when an active dataset is stored; otherwise Thread stays idle.
 *
 * Blocks until OpenThread is initialised or the radio fault mode was entered, and returns ESP_OK in both
 * cases (a radio fault is reported through br_core_get_mode: fault_reason "no rcp firmware" for an empty C6 boot
 * slot, "rcp incompatible" when OpenThread rejects the RCP). The border router start itself may come later.
 * Call it from a task pinned to core 0 after esp_netif_init(), the default event loop and the storage mounts.
 *
 * @param backbone the Ethernet netif (the backbone interface); must not be NULL
 * A failing mDNS start is logged and does not stop the radio start (the board is then reachable by address only).
 * @return ESP_ERR_INVALID_ARG for NULL, ESP_ERR_INVALID_STATE if called twice, ESP_ERR_NO_MEM
 */
esp_err_t br_core_start(esp_netif_t *backbone);

br_core_mode_t br_core_get_mode(void);

/** True only in BR_MODE_BR_RUNNING: the REST guard allows Thread operations. */
bool br_core_thread_ops_allowed(void);

/** Snapshot of the mode and, when OpenThread is up, of the dataset and DHCPv6-PD state (ot_state_valid says whether they were read). Never blocks long. */
void br_core_get_status(br_core_status_t *out);

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
 * Push an RCP image to the C6 on request (R14): stop OpenThread, rcp_sync_push_firmware, hardware-reset the C6,
 * probe again (rcp_installed shows the pushed version) and restart OpenThread and the border router. Works from
 * BR_RUNNING and RADIO_FAULT (also "no rcp firmware"); leaves RADIO_FAULT on success. A failed push restarts
 * OpenThread with the previous firmware and returns the push error. Blocks for seconds to minutes; it runs on
 * the caller's (management) task and takes the OpenThread lock only around the stop/start. Not callable from
 * the OpenThread task. The Thread REST API is switched off before OpenThread stops.
 * ESP_ERR_INVALID_STATE in SNIFFER and STARTING, or while another push runs (nothing is started then);
 * ESP_ERR_INVALID_ARG for a NULL source or read. `err_detail` (may be NULL) gets a short reason on failure.
 * Refused with ESP_ERR_INVALID_STATE as well when called from the OpenThread task, before anything is touched.
 * Refusal in STARTING also covers a border router start that failed (the mode stays STARTING): reboot instead.
 * After an abandoned OpenThread start ("rcp incompatible": OpenThread's SPI driver stays registered and cannot be
 * released) the push runs without stopping OpenThread and a successful push ends in a reboot ~1.5 s later (the
 * return value is ESP_OK, the mode stays RADIO_FAULT until then); a failed push leaves everything as it was.
 */
esp_err_t br_core_push_radio_firmware(const rcp_fw_source_t *source, char *err_detail, size_t err_cap);

/**
 * ROM recovery of the C6 base image on request (spec 5.10, replaces the former resync-with-recovery): the same
 * discipline as br_core_push_radio_firmware (one C6 job at a time, the Thread REST API off before OpenThread stops
 * and restored from the final state, refused from the OpenThread task, an abandoned start recovers without stopping
 * OpenThread and ends in a reboot). OpenThread stops, rcp_sync_rom_recover rewrites bootloader, partition table and
 * updater (no RCP firmware), the probe reads the result and OpenThread restarts: with the RCP that survived in its
 * slot the BR runs again, otherwise the mode is RADIO_FAULT "no rcp firmware" until firmware is uploaded.
 * ESP_ERR_NOT_SUPPORTED without the jumper (nothing is touched), ESP_ERR_INVALID_STATE in SNIFFER and STARTING or
 * while another C6 job runs. A failed recovery restarts OpenThread (the boot probe decides the mode) and returns the error.
 * `err_detail` (may be NULL) gets a short reason on failure. Blocks for tens of seconds.
 */
esp_err_t br_core_rom_recover(char *err_detail, size_t err_cap);

/**
 * Start the app validity timer: once the image is pending-verify, the app has run 60 s and the HTTP server
 * is listening (esp_br_web_is_server_started), mark it valid. Radio state, Ethernet link, sync result and
 * OpenThread state are deliberately not criteria. Safe to call early: the 60 s count from boot.
 */
void validity_start(void);

#ifdef OTBR_TEST_BUILD
/** Test builds: hold the C6 in reset instead of booting the RCP (persisted in NVS across reboots). */
esp_err_t br_core_test_set_c6_hold(bool hold);
bool br_core_test_c6_hold(void);

typedef enum {
    BR_CORE_TEST_PROBE_REAL, /**< run the real probe */
    BR_CORE_TEST_PROBE_EMPTY, /**< pretend the C6 boot slot is empty */
    BR_CORE_TEST_PROBE_FAIL, /**< pretend the probe failed */
} br_core_test_probe_t;

/** Test builds: force the boot-time probe result (persisted in NVS across reboots). */
esp_err_t br_core_test_set_probe_override(br_core_test_probe_t mode);
br_core_test_probe_t br_core_test_probe_override(void);

/**
 * Test builds: register the console command `ot` (call once the console is initialised). While OpenThread's CLI runs it
 * is ESP-IDF's OpenThread CLI command; otherwise a stand-in that prints `Error 13: InvalidState (OpenThread not running)`,
 * because the CLI command posts to OpenThread's task queue, which exists only between esp_openthread_init and
 * esp_openthread_deinit (posting outside that asserts).
 */
esp_err_t br_core_test_ot_command_register(void);

/** Test builds, called by br_core: OpenThread's CLI started (after esp_openthread_cli_init) or is about to stop. */
void br_core_test_ot_command_set_running(bool running);
#endif

#ifdef __cplusplus
}
#endif
