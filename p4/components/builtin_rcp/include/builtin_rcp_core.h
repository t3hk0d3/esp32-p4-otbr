// Decisions of the board's built-in RCP (the C6 on this board). Pure: no ESP-IDF calls.
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "radio_mux_orch_core.h"
#include "rcp_sync.h"

#ifdef __cplusplus
extern "C" {
#endif

// Whether the C6 is usable after rcp_sync's last `probe`: not when it runs the factory ESP-Hosted firmware, when a ROM
// recovery failed and the updater stayed silent, or when the updater answered with no RCP installed; then *kind (may
// be NULL) is RADIO_MUX_FAULT_FIRMWARE and `reason` (may be NULL) names which. Otherwise *kind is NONE and `reason`
// empty. No probe yet, or a silent updater without a failed recovery: usable (the installed firmware is tried).
bool builtin_rcp_present(const rcp_sync_probe_result_t *probe, radio_mux_fault_kind_t *kind, char *reason, size_t cap);

// What a failed ROM recovery in the boot probe means: a fault ("rom recovery failed") only when the updater was silent
// before the attempt. When it was reachable (the trigger was a base version mismatch) an RCP may be installed and the
// boot goes on with it; the failure is only recorded in the status. False when no recovery failed.
bool builtin_rcp_rom_failure_is_fault(const rcp_sync_probe_result_t *probe);

// The sticky "a ROM recovery failed" flag of the status: unchanged without an attempt, set by a failed attempt, cleared
// by a successful one. Applies to automatic (boot probe) and manual recoveries alike.
bool builtin_rcp_next_recovery_failed(bool previous, bool attempted, bool succeeded);

// What the status shows as the installed RCP version after a push whose version was read back from the C6 (`probe_ok`)
// or not: the read-back version, or, when it could not be read, unknown (*known false, `out` empty), never the version
// from before the push.
void builtin_rcp_installed_after_push(bool probe_ok, const char *probed_version, bool *known, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
