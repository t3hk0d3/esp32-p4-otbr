#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * HTTP surface notes (spec 5.7):
 * - POST /device/radio/firmware 200 body is {"installed": "<version>"}; "installed" is null when the version could not be
 *   read back from the C6 (never a stale value: after an abandoned-start push br_core re-reads it from the updater first).
 * - Upload size is checked before anything is torn down: missing length 400 "length_required", more than the C6 slot
 *   (C6_OTA_SLOT_SIZE) 413 "payload_too_large"; the same bound applies to from_url after the response headers.
 *   POST /device/ota is bounded by header + web + app partition sizes (413).
 * - from_url / ota url must be http://; https gives 400 "https_not_supported" (no certificate bundle).
 * - POST /device/radio/recover: 200 {"recovered":true,"rcp":"<installed version>"|"no rcp firmware"|"unknown","mode":...,"detail":...}
 *   once the base image was rewritten (also when the restart then finds no RCP firmware: RADIO_FAULT is the state, not an error);
 *   502 "recovery_failed" only when the recovery failed; 412 "no_jumper"
 *   without the maintenance jumper; 409 in SNIFFER/STARTING or while an OTA, push or recovery runs. GET /device/radio/log
 *   is text/plain (the newest 16 KB of C6 console), 412 without the jumper.
 * - GET /device/diag (spec 5.12) serves diag's precomputed snapshot in every mode; see diag_handler.h. Its `ot_lock` figures cover the
 *   wrapped esp_openthread_lock_* callers (libopenthread, br_core, Thread REST), not the border router internals that use only the
 *   task-switching lock (see diag.h). 503 "not_started" when diag_start failed at boot.
 * - POST /device/reboot answers 409 while an OTA or firmware push runs (or a committed OTA waits for its reboot).
 *
 * - /device/network (spec 5.13): GET = netcfg_state_json; PUT = JSON network config (body at most NETCFG_JSON_MAX bytes: 400
 *   "length_required", 413 "payload_too_large"), 400 {"error":"invalid_config","detail":"<field>"} for an invalid one (nothing is
 *   stored), 409 "busy" while an OTA or push runs, else 200 and a reboot into the trial; POST /device/network/confirm = 200, 409
 *   "no_trial", 403 "wrong_interface". In split mode every /device/ route answers 403 and closes the connection on thread_bb.
 *
 * Register the /device/ handlers (spec 5.7: status, sniffer.pcap, sniffer, ota, ota/url, radio/firmware, radio/recover, radio/log, reboot, diag, network) on
 * `server`. Needs no OpenThread: the handlers also work in RADIO_FAULT, before OpenThread is up.
 * ESP_ERR_INVALID_ARG for NULL; otherwise the httpd_register_uri_handler error.
 */
esp_err_t device_api_register(httpd_handle_t server);

/**
 * Tell device_api the version text of the C6 base image embedded in the app (bytes [start, end), no terminator), shown as
 * jumper.base_expected in /device/status. The embedded blob belongs to main, hence this setter. Call before the server
 * starts; without it base_expected is empty.
 */
void device_api_set_base_expected(const uint8_t *start, const uint8_t *end);

/**
 * True while an OTA, a radio firmware push or a ROM recovery runs (or a committed OTA waits for its own reboot): the
 * predicate of the reboot gate (reboot_gate_set_busy), so that other reboots wait for it. Safe from any task.
 */
bool device_api_long_op_running(void);

#ifdef OTBR_TEST_BUILD
/** Test builds: reset the C6 once, when a radio firmware push has read up to `offset` bytes of its source (0 = off). */
void device_api_test_inject_c6_reset_at(uint32_t offset);
#endif

#ifdef __cplusplus
}
#endif
