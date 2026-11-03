#pragma once

// Liveness supervisor: aborts (core dump, then reboot) when the OpenThread lock or the HTTP server stays stuck, which
// the idle-task watchdog cannot see (the idle tasks keep running while two tasks wait on each other). Two probes,
// every SUPERVISOR_PERIOD_S:
// - OpenThread lock: inside a br_core session (none during a radio push, so the push's teardown is never probed), the
//   lock must be taken within SUPERVISOR_OT_LOCK_WAIT_S; two failures in a row abort.
// - HTTP server: a no-op is queued to the server task (httpd_queue_work); it must run within SUPERVISOR_HTTPD_STALL_S.
// The limits are far above the longest legitimate wait (an HTTP handler waits at most 30 s for OpenThread).

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SUPERVISOR_PERIOD_S 10
#define SUPERVISOR_OT_LOCK_WAIT_S 60
#define SUPERVISOR_HTTPD_STALL_S 120

/** Starts the supervisor task (management core). Call once, after the HTTP server was started. */
esp_err_t supervisor_start(void);

#ifdef __cplusplus
}
#endif
