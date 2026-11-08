// Decisions of the board's built-in RCP. Pure: no ESP-IDF calls.
#include "builtin_rcp_core.h"

#include <stdio.h>
#include <string.h>

bool builtin_rcp_present(const rcp_sync_probe_result_t *probe, radio_mux_fault_kind_t *kind, char *reason, size_t cap)
{
    const char *why = NULL;

    if (probe->esp_hosted) {
        why = "the C6 runs its factory ESP-Hosted firmware";
    } else if (probe->rom_recovery_failed && !probe->updater_reachable) {
        why = "the ROM recovery of the C6 failed";
    } else if (probe->updater_reachable && !probe->installed_valid) {
        why = "no RCP firmware on the C6";
    }
    if (kind) {
        *kind = why ? RADIO_MUX_FAULT_FIRMWARE : RADIO_MUX_FAULT_NONE;
    }
    if (reason && cap > 0) {
        snprintf(reason, cap, "%s", why ? why : "");
    }
    return why == NULL;
}

bool builtin_rcp_rom_failure_is_fault(const rcp_sync_probe_result_t *probe)
{
    return probe->rom_recovery_failed && !probe->updater_reachable;
}

bool builtin_rcp_next_recovery_failed(bool previous, bool attempted, bool succeeded)
{
    return attempted ? !succeeded : previous;
}

void builtin_rcp_installed_after_push(bool probe_ok, const char *probed_version, bool *known, char *out, size_t cap)
{
    *known = probe_ok;
    if (cap == 0) {
        return;
    }
    out[0] = '\0';
    if (probe_ok && probed_version) {
        strlcpy(out, probed_version, cap);
    }
}
