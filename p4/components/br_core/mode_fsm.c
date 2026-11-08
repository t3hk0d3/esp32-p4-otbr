#include "mode_fsm.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"

static const char *TAG = "mode_fsm";

static const char *mode_name(br_core_mode_t mode)
{
    switch (mode) {
    case BR_MODE_STARTING:
        return "STARTING";
    case BR_MODE_BR_RUNNING:
        return "BR_RUNNING";
    case BR_MODE_SNIFFER:
        return "SNIFFER";
    case BR_MODE_RADIO_FAULT:
        return "RADIO_FAULT";
    case BR_MODE_NO_RADIO:
        return "NO_RADIO";
    }
    return "?";
}

// Every change of the mode is logged and reported (the mode_changed op).
static void changed(mode_fsm_t *fsm, br_core_mode_t mode)
{
    ESP_LOGI(TAG, "mode %s", mode_name(mode));
    if (fsm->ops->mode_changed) {
        fsm->ops->mode_changed(mode);
    }
}

static void set_mode(mode_fsm_t *fsm, br_core_mode_t mode)
{
    atomic_store(&fsm->mode, mode);
    changed(fsm, mode);
}

// Back to the state before the capture: dataset channel, OpenThread netif, Thread. Any failure reboots.
static esp_err_t restore_thread(mode_fsm_t *fsm)
{
    const mode_fsm_ops_t *ops = fsm->ops;
    esp_err_t err = ops->restore_channel();

    if (err == ESP_OK) {
        err = ops->netif_up();
    }
    if (err == ESP_OK) {
        err = ops->thread_start();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "restoring Thread failed (%s), rebooting", esp_err_to_name(err));
        ops->reboot();
    } else {
        fsm->thread_stopped_by_capture = false;
    }
    return err;
}

void mode_fsm_init(mode_fsm_t *fsm, const mode_fsm_ops_t *ops)
{
    fsm->ops = ops;
    fsm->recovery_count = 0;
    fsm->sniffer_end_cb = NULL;
    fsm->thread_stopped_by_capture = false;
    atomic_init(&fsm->mode, BR_MODE_STARTING);
}

br_core_mode_t mode_fsm_get_mode(const mode_fsm_t *fsm)
{
    return atomic_load(&fsm->mode);
}

bool mode_fsm_thread_ops_allowed(const mode_fsm_t *fsm)
{
    return mode_fsm_get_mode(fsm) == BR_MODE_BR_RUNNING;
}

bool mode_fsm_set_started(mode_fsm_t *fsm)
{
    br_core_mode_t expected = BR_MODE_STARTING;
    const bool started = atomic_compare_exchange_strong(&fsm->mode, &expected, BR_MODE_BR_RUNNING);

    if (started) {
        changed(fsm, BR_MODE_BR_RUNNING);
    }
    return started;
}

void mode_fsm_set_sniffer_end_cb(mode_fsm_t *fsm, void (*cb)(void))
{
    fsm->sniffer_end_cb = cb;
}

static void notify_sniffer_end(const mode_fsm_t *fsm)
{
    if (fsm->sniffer_end_cb != NULL) {
        fsm->sniffer_end_cb();
    }
}

void mode_fsm_set_fault(mode_fsm_t *fsm)
{
    const br_core_mode_t before = atomic_exchange(&fsm->mode, BR_MODE_RADIO_FAULT);

    changed(fsm, BR_MODE_RADIO_FAULT);
    if (before == BR_MODE_SNIFFER) {
        notify_sniffer_end(fsm);
    }
}

esp_err_t mode_fsm_enter_sniffer(mode_fsm_t *fsm, uint8_t channel, otLinkRawReceiveDone cb)
{
    const mode_fsm_ops_t *ops = fsm->ops;

    if (mode_fsm_get_mode(fsm) != BR_MODE_BR_RUNNING) {
        return ESP_ERR_INVALID_STATE;
    }
    if (channel < MODE_FSM_MIN_CHANNEL || channel > MODE_FSM_MAX_CHANNEL || cb == NULL) {
        return ESP_ERR_INVALID_ARG; // a NULL callback would switch the raw link off again: nothing is touched
    }
    esp_err_t err = ops->thread_stop();
    if (err == ESP_OK) {
        err = ops->netif_down();
    }
    if (err == ESP_OK) {
        err = ops->raw_enable(channel, cb);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "entering the sniffer failed (%s), back to the border router", esp_err_to_name(err));
        // Thread was stopped (and maybe the netif): both come back. Those ops do nothing for what they did not stop.
        if (ops->netif_up() != ESP_OK || ops->thread_start() != ESP_OK) {
            ESP_LOGE(TAG, "restoring Thread failed, rebooting");
            ops->reboot();
        }
        return err;
    }
    fsm->thread_stopped_by_capture = true;
    set_mode(fsm, BR_MODE_SNIFFER);
    return ESP_OK;
}

esp_err_t mode_fsm_leave_sniffer(mode_fsm_t *fsm)
{
    const mode_fsm_ops_t *ops = fsm->ops;

    if (mode_fsm_get_mode(fsm) != BR_MODE_SNIFFER) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = ops->raw_disable();
    if (err == ESP_OK) {
        err = restore_thread(fsm);
    } else {
        ESP_LOGE(TAG, "disabling the raw link failed (%s), rebooting", esp_err_to_name(err));
        ops->reboot();
    }
    if (err == ESP_OK) {
        set_mode(fsm, BR_MODE_BR_RUNNING);
    }
    return err;
}

bool mode_fsm_storage_degraded(esp_err_t storage_init_err)
{
    return storage_init_err != ESP_OK;
}

bool mode_fsm_radio_start_allowed(bool storage_degraded)
{
    return !storage_degraded;
}

mode_fsm_fault_action_t mode_fsm_fault_action(bool caller_is_openthread_task)
{
    return caller_is_openthread_task ? MODE_FSM_FAULT_PARK_TASK : MODE_FSM_FAULT_REBOOT;
}

// Drop the recoveries older than the window; true when another one may start.
static bool recovery_allowed(mode_fsm_t *fsm, int64_t now)
{
    size_t kept = 0;

    for (size_t i = 0; i < fsm->recovery_count; i++) {
        if (now - fsm->recovery_us[i] < MODE_FSM_RECOVERY_WINDOW_US) {
            fsm->recovery_us[kept++] = fsm->recovery_us[i];
        }
    }
    fsm->recovery_count = kept;
    return kept < MODE_FSM_MAX_RECOVERIES;
}

// Runs on the OpenThread task once the failure hook has returned and OpenThread has restored the RCP's state.
static void finish_sniffer_recovery(void *ctx)
{
    mode_fsm_t *fsm = ctx;

    if (mode_fsm_get_mode(fsm) != BR_MODE_RADIO_FAULT) {
        return; // something else (a fault, a push) took over meanwhile
    }
    if (fsm->ops->quiet_radio() != ESP_OK) {
        ESP_LOGW(TAG, "quieting the radio after the capture failed");
    }
    if (restore_thread(fsm) == ESP_OK) {
        set_mode(fsm, BR_MODE_BR_RUNNING);
    }
}

esp_err_t mode_fsm_reset_rcp(mode_fsm_t *fsm)
{
    const mode_fsm_ops_t *ops = fsm->ops;
    const esp_err_t err = ops->reset_rcp();

    if (err != ESP_ERR_TIMEOUT && err != ESP_ERR_NO_MEM) {
        return err;
    }
    ESP_LOGW(TAG, "the RCP reset was not taken (%s): trying once more", esp_err_to_name(err));
    ops->sleep_ms(MODE_FSM_RESET_RETRY_MS);
    return ops->reset_rcp();
}

void mode_fsm_on_rcp_failure(mode_fsm_t *fsm)
{
    const mode_fsm_ops_t *ops = fsm->ops;
    const br_core_mode_t mode = mode_fsm_get_mode(fsm);

    if (mode == BR_MODE_STARTING) {
        mode_fsm_reset_rcp(fsm);
        return;
    }
    if (mode == BR_MODE_SNIFFER) {
        if (ops->raw_abort() != ESP_OK) { // the capture ends; no request to the RCP here, it would only time out again
            ESP_LOGW(TAG, "dropping the raw link failed");
        }
    }
    set_mode(fsm, BR_MODE_RADIO_FAULT);
    if (mode == BR_MODE_SNIFFER) {
        notify_sniffer_end(fsm);
    }

    const int64_t now = ops->now_us();
    if (!recovery_allowed(fsm, now)) {
        ESP_LOGE(TAG, "%d recoveries in the last 10 minutes, no automatic recovery", MODE_FSM_MAX_RECOVERIES);
        return;
    }
    fsm->recovery_us[fsm->recovery_count++] = now;

    if (mode_fsm_reset_rcp(fsm) != ESP_OK) {
        ESP_LOGE(TAG, "resetting the RCP failed, staying in the radio fault");
        return;
    }
    if (fsm->thread_stopped_by_capture) { // Thread is stopped (this or an earlier round): back once the RCP answers
        if (ops->defer(finish_sniffer_recovery, fsm) != ESP_OK) {
            ESP_LOGE(TAG, "cannot schedule the restore, rebooting");
            ops->reboot();
        }
        return;
    }
    set_mode(fsm, BR_MODE_BR_RUNNING);
}

esp_err_t mode_fsm_radio_job_result(bool base_only, esp_err_t job_err, esp_err_t restart_err)
{
    if (base_only || job_err != ESP_OK) {
        return job_err;
    }
    return restart_err == ESP_OK ? ESP_OK : RCP_SYNC_ERR_RESTART;
}

bool mode_fsm_boot(mode_fsm_t *fsm, const char *otbr_id)
{
    if (otbr_id && strcmp(otbr_id, MODE_FSM_BUILTIN_ID) == 0) {
        return true; // the C6 boots as it always did
    }
    set_mode(fsm, BR_MODE_NO_RADIO);
    return false;
}

static bool is_c6(const char *id)
{
    return id && strcmp(id, MODE_FSM_BUILTIN_ID) == 0;
}

bool mode_fsm_probe_fault_incompatible(const char *id, esp_err_t probe_err)
{
    return is_c6(id) && probe_err != ESP_OK && probe_err != ESP_ERR_INVALID_STATE;
}

bool mode_fsm_start_failed(br_core_mode_t mode, bool ot_running)
{
    return mode == BR_MODE_RADIO_FAULT && !ot_running;
}

// STARTING, then OpenThread on `id`. The border router start sets BR_RUNNING (mode_fsm_set_started).
static esp_err_t start_on(mode_fsm_t *fsm, const char *id)
{
    set_mode(fsm, BR_MODE_STARTING);
    const esp_err_t err = fsm->ops->ot_start(id);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "starting OpenThread on %s failed: %s", id, esp_err_to_name(err));
    }
    return err;
}

// A move whose commit failed changed nothing: OpenThread starts again on the source, still the channel stored in otbr
// (NO_RADIO with none to go back to: absent, or a start). Never after a failed start on the target: that start's
// binding stays. MODE_FSM_ERR_START_FAILED.
static esp_err_t resume_on_source(mode_fsm_t *fsm, br_core_mode_t prev, const char *from)
{
    if (prev == BR_MODE_NO_RADIO || from[0] == '\0') {
        set_mode(fsm, BR_MODE_NO_RADIO);
        return MODE_FSM_ERR_START_FAILED;
    }
    if (start_on(fsm, from) != ESP_OK) {
        mode_fsm_set_fault(fsm);
    }
    return MODE_FSM_ERR_START_FAILED;
}

esp_err_t mode_fsm_move_otbr(mode_fsm_t *fsm, const char *from, const char *to, char *err, size_t cap)
{
    const mode_fsm_ops_t *ops = fsm->ops;
    const br_core_mode_t prev = mode_fsm_get_mode(fsm);

    if (cap > 0) {
        err[0] = '\0';
    }
    if (prev == BR_MODE_STARTING || prev == BR_MODE_SNIFFER) {
        snprintf(err, cap, "OpenThread is busy (%s), try again", mode_name(prev));
        return ESP_ERR_INVALID_STATE;
    }
    // 1. The probe, while OpenThread still runs on the source (a stop has no target: the op only accepts it).
    if (ops->probe_channel(to, err, cap) != ESP_OK) {
        ESP_LOGW(TAG, "moving OpenThread to %s: the probe failed: %s", to, err);
        return MODE_FSM_ERR_PROBE_FAILED;
    }
    // 2. Stop on the source (nothing runs there when it is absent, NO_RADIO, or for a start).
    set_mode(fsm, BR_MODE_STARTING);
    if (ops->ot_stop() != ESP_OK) {
        set_mode(fsm, prev);
        snprintf(err, cap, "%s", MODE_FSM_DRAIN_TIMEOUT);
        ESP_LOGW(TAG, "%s %s: the sessions did not end, nothing changed", to[0] ? "moving OpenThread to" :
                 "stopping OpenThread on", to[0] ? to : from);
        return MODE_FSM_ERR_DRAIN_TIMEOUT;
    }
    // 3. The commit: the binding moves (both stored modes at once). A failure changed nothing.
    if (ops->store_otbr(from, to) != ESP_OK) {
        if (to[0] == '\0') {
            snprintf(err, cap, "storing the mode failed: OpenThread starts again on %s", from);
        } else {
            snprintf(err, cap, "storing the modes failed: OpenThread goes back to %s", from[0] ? from : "no radio");
        }
        return resume_on_source(fsm, prev, from);
    }
    if (to[0] == '\0') { // a stop
        set_mode(fsm, BR_MODE_NO_RADIO);
        return ESP_OK;
    }
    // 4. Start on the target, now Thread's channel. A failure stays there (RADIO_FAULT): no rollback to the source; a
    // PUT of mode otbr on the target retries, one on another channel moves it.
    const esp_err_t start_err = start_on(fsm, to);

    if (start_err == ESP_OK) {
        return ESP_OK;
    }
    if (start_err == MODE_FSM_ERR_START_ABANDONED) {
        // Nothing starts OpenThread again in this boot: RADIO_FAULT, as on every other path (no reboot).
        snprintf(err, cap, "%s", MODE_FSM_FAULT_ABANDONED);
    } else {
        snprintf(err, cap, "OpenThread did not start on %s", to);
    }
    mode_fsm_set_fault(fsm);
    return start_err == MODE_FSM_ERR_START_ABANDONED ? start_err : MODE_FSM_ERR_START_FAILED;
}

void mode_fsm_on_channel_gone(mode_fsm_t *fsm)
{
    // NO_RADIO before the teardown: the guards refuse new Thread sessions (503) at once while the open ones drain.
    const br_core_mode_t before = atomic_exchange(&fsm->mode, BR_MODE_NO_RADIO);

    ESP_LOGI(TAG, "the OTBR channel is gone");
    changed(fsm, BR_MODE_NO_RADIO);
    if (before == BR_MODE_SNIFFER) {
        fsm->thread_stopped_by_capture = false; // OpenThread goes away: nothing to restore
        notify_sniffer_end(fsm);
    }
    if (fsm->ops->ot_stop() != ESP_OK) {
        ESP_LOGE(TAG, "tearing OpenThread down after the OTBR channel went failed");
        mode_fsm_set_fault(fsm);
    } else {
        // OpenThread is gone: NO_RADIO again, also when its task parked itself into RADIO_FAULT during the drain.
        set_mode(fsm, BR_MODE_NO_RADIO);
    }
}

void mode_fsm_on_channel_back(mode_fsm_t *fsm, const char *id)
{
    char err[64];

    set_mode(fsm, BR_MODE_STARTING);
    if (fsm->ops->probe_channel(id, err, sizeof(err)) != ESP_OK) {
        ESP_LOGE(TAG, "%s: the RCP probe failed: %s", id, err);
        mode_fsm_set_fault(fsm);
        return;
    }
    if (start_on(fsm, id) != ESP_OK) {
        mode_fsm_set_fault(fsm);
    }
}

br_core_mode_t mode_fsm_mode_for(const mode_fsm_view_t *v, char *reason, size_t cap)
{
    const char *id = v->chan_id ? v->chan_id : "";
    const char *cr = v->chan_reason ? v->chan_reason : "";
    const char *tr = v->thread_reason ? v->thread_reason : "";

    reason[0] = '\0';
    if (!v->bound) {
        snprintf(reason, cap, "no channel in otbr");
        return BR_MODE_NO_RADIO;
    }
    if (v->thread == BR_MODE_STARTING) {
        return BR_MODE_STARTING;
    }
    if (v->chan_state == RADIO_MUX_CS_READY) {
        switch (v->thread) {
        case BR_MODE_BR_RUNNING:
        case BR_MODE_SNIFFER:
            return v->thread;
        case BR_MODE_RADIO_FAULT:
            snprintf(reason, cap, "%s", tr);
            return BR_MODE_RADIO_FAULT;
        default: // bound but not started: waiting for the start, or the firmware does not fit
            snprintf(reason, cap, tr[0] ? "%s" : "waiting for %s", tr[0] ? tr : id);
            return BR_MODE_NO_RADIO;
        }
    }
    switch (v->chan_state) {
    case RADIO_MUX_CS_FAULT:
        if (v->kind == RADIO_MUX_FAULT_FIRMWARE) {
            snprintf(reason, cap, "%s", cr);
            return BR_MODE_RADIO_FAULT;
        }
        snprintf(reason, cap, "%s (%s)", cr, id);
        return BR_MODE_NO_RADIO;
    case RADIO_MUX_CS_REINIT:
        if (v->work != RADIO_MUX_WORK_DETECT) {
            return BR_MODE_STARTING; // the built-in RCP's maintenance: Thread starts once it ends
        }
        snprintf(reason, cap, "waiting for %s", id);
        return BR_MODE_NO_RADIO;
    default: // absent
        snprintf(reason, cap, v->unplugged ? "radio unplugged (%s)" : "waiting for %s", id);
        return BR_MODE_NO_RADIO;
    }
}

esp_err_t mode_fsm_reinit_verdict(br_core_mode_t mode)
{
    return mode == BR_MODE_SNIFFER || mode == BR_MODE_STARTING ? ESP_ERR_INVALID_STATE : ESP_OK;
}

bool mode_fsm_input_newer(uint32_t seen_seq, uint32_t input_seq)
{
    return (int32_t)(input_seq - seen_seq) > 0;
}

esp_err_t mode_fsm_on_reinit(mode_fsm_t *fsm)
{
    const br_core_mode_t prev = mode_fsm_get_mode(fsm);
    const esp_err_t verdict = mode_fsm_reinit_verdict(prev);

    if (verdict != ESP_OK) {
        return verdict;
    }
    set_mode(fsm, BR_MODE_STARTING); // the guards refuse new sessions while the open ones drain
    if (fsm->ops->ot_stop() != ESP_OK) {
        set_mode(fsm, prev); // nothing stopped: OpenThread goes on as before
        return MODE_FSM_ERR_DRAIN_TIMEOUT;
    }
    return ESP_OK;
}
