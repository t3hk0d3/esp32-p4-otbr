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

esp_err_t builtin_rcp_job_result(bool base_only, esp_err_t job_err, radio_mux_settle_t settle)
{
    if (base_only || job_err != ESP_OK) {
        return job_err;
    }
    return settle == RADIO_MUX_SETTLE_DOWN || settle == RADIO_MUX_SETTLE_WAIT ? RCP_SYNC_ERR_RESTART : ESP_OK;
}

static const char *who(radio_mux_mode_t mode)
{
    switch (mode) {
    case RADIO_MUX_MODE_OTBR:
        return "OpenThread";
    case RADIO_MUX_MODE_ZIGGURAT:
        return "Zigbee";
    default:
        return radio_mux_mode_name(mode);
    }
}

void builtin_rcp_restart_detail(char *out, size_t cap, const radio_mux_maint_t *m)
{
    char waited[48];
    const char *why = m->mode_reason[0] ? m->mode_reason : m->mode_state;

    if (cap == 0) {
        return;
    }
    if (m->settle == RADIO_MUX_SETTLE_WAIT) {
        snprintf(waited, sizeof(waited), "still starting after %d s", RADIO_MUX_SETTLE_MS / 1000);
        why = waited;
    }
    snprintf(out, cap, "the image is installed, but %s did not start on it: %s", who(m->mode), why);
}
