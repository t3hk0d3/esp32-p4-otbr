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
    fsm->radio_job = false;
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
    if (fsm->radio_job) {
        return false; // the restart inside a push or ROM recovery: the job sets the final mode when it ends
    }
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

esp_err_t mode_fsm_push_allowed(br_core_mode_t mode, bool caller_is_openthread_task)
{
    if (caller_is_openthread_task || mode == BR_MODE_SNIFFER || mode == BR_MODE_STARTING) {
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
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

bool mode_fsm_rest_ready_after_push(br_core_mode_t mode, bool openthread_ready)
{
    return mode == BR_MODE_BR_RUNNING && openthread_ready;
}

esp_err_t mode_fsm_begin_radio_job(mode_fsm_t *fsm)
{
    const br_core_mode_t mode = mode_fsm_get_mode(fsm);

    if (fsm->radio_job || mode_fsm_push_allowed(mode, false) != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    fsm->radio_job = true;
    fsm->radio_job_prev = mode;
    set_mode(fsm, BR_MODE_STARTING); // OpenThread is stopped and started again: nothing may use it until then
    return ESP_OK;
}

void mode_fsm_cancel_radio_job(mode_fsm_t *fsm)
{
    if (fsm->radio_job) {
        fsm->radio_job = false;
        br_core_mode_t expected = BR_MODE_STARTING; // a fault entered meanwhile stays
        if (atomic_compare_exchange_strong(&fsm->mode, &expected, fsm->radio_job_prev)) {
            changed(fsm, fsm->radio_job_prev);
        }
    }
}

// The job of a radio request (a firmware push, a ROM recovery): `source` set = push, NULL = ROM recovery.
static esp_err_t run_radio_job(mode_fsm_t *fsm, const rcp_fw_source_t *source, char *err_detail, size_t err_cap)
{
    bool restarted = false;

    if (!fsm->radio_job && mode_fsm_begin_radio_job(fsm) != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = source ? fsm->ops->push_firmware(source, err_detail, err_cap, &restarted)
                    : fsm->ops->rom_recover(err_detail, err_cap, &restarted);
    if (restarted) {
        fsm->thread_stopped_by_capture = false; // OpenThread started afresh
    }
    fsm->radio_job = false;
    set_mode(fsm, restarted ? BR_MODE_BR_RUNNING : BR_MODE_RADIO_FAULT);
    return err;
}

esp_err_t mode_fsm_push_firmware(mode_fsm_t *fsm, const rcp_fw_source_t *source, char *err_detail, size_t err_cap)
{
    if (!source) {
        return ESP_ERR_INVALID_ARG; // NULL would select the ROM recovery job
    }
    return run_radio_job(fsm, source, err_detail, err_cap);
}

esp_err_t mode_fsm_rom_recover(mode_fsm_t *fsm, char *err_detail, size_t err_cap)
{
    return run_radio_job(fsm, NULL, err_detail, err_cap);
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
    if (otbr_id && strcmp(otbr_id, MODE_FSM_C6_ID) == 0) {
        return true; // the C6 boots as it always did
    }
    set_mode(fsm, BR_MODE_NO_RADIO);
    return false;
}

bool mode_fsm_c6_usable_after_steps(esp_err_t steps)
{
    return steps == ESP_OK;
}

static bool is_c6(const char *id)
{
    return id && strcmp(id, MODE_FSM_C6_ID) == 0;
}

bool mode_fsm_probe_fault_incompatible(const char *id, esp_err_t probe_err)
{
    return is_c6(id) && probe_err != ESP_OK && probe_err != ESP_ERR_INVALID_STATE;
}

bool mode_fsm_c6_job_alone(const char *otbr_id)
{
    return !is_c6(otbr_id);
}

bool mode_fsm_job_holds_openthread(bool job_running, const char *ot_chan_id, const char *otbr_id)
{
    return job_running && (is_c6(ot_chan_id) || is_c6(otbr_id));
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

// 5. of a move: back on the source, which keeps its stored mode; NO_RADIO when there is none to go back to (absent:
// the mode before was NO_RADIO; a start: no source). MODE_FSM_ERR_START_FAILED, or ..._START_ABANDONED.
static esp_err_t start_back(mode_fsm_t *fsm, br_core_mode_t prev, const char *from, const char *to, char *err,
                            size_t cap)
{
    if (prev == BR_MODE_NO_RADIO || from[0] == '\0') {
        set_mode(fsm, BR_MODE_NO_RADIO);
        return MODE_FSM_ERR_START_FAILED;
    }
    const esp_err_t back_err = start_on(fsm, from);

    if (back_err == MODE_FSM_ERR_START_ABANDONED) {
        // As on the target: nothing starts OpenThread again in this boot; a reboot brings it back on `from`.
        snprintf(err, cap, "OpenThread did not start on %s, nor again on %s: rebooting", to[0] ? to : "(none)", from);
        return MODE_FSM_ERR_START_ABANDONED;
    }
    if (back_err != ESP_OK) {
        snprintf(err, cap, "OpenThread did not start on %s, nor again on %s", to[0] ? to : "(none)", from);
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
    if (fsm->radio_job || prev == BR_MODE_STARTING || prev == BR_MODE_SNIFFER) {
        snprintf(err, cap, "OpenThread is busy (%s), try again", fsm->radio_job ? "radio job" : mode_name(prev));
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
    if (to[0] == '\0') {
        // A stop: the source's new mode is stored only now that OpenThread stopped.
        if (ops->store_otbr(from, "") == ESP_OK) {
            set_mode(fsm, BR_MODE_NO_RADIO);
            return ESP_OK;
        }
        snprintf(err, cap, "storing the mode failed: OpenThread starts again on %s", from);
        return start_back(fsm, prev, from, to, err, cap);
    }
    // 3. Start on the target; 4. store both modes only once it runs.
    const esp_err_t start_err = start_on(fsm, to);

    if (start_err == MODE_FSM_ERR_START_ABANDONED) {
        // Nothing starts OpenThread again in this boot: the board reboots, on the source's stored mode.
        snprintf(err, cap, "OpenThread could not start on %s: rebooting, Thread comes back on %s", to,
                 from[0] ? from : "no radio");
        return MODE_FSM_ERR_START_ABANDONED;
    }
    if (start_err == ESP_OK) {
        if (ops->store_otbr(from, to) == ESP_OK) {
            return ESP_OK;
        }
        snprintf(err, cap, "storing the modes failed: OpenThread goes back to %s", from[0] ? from : "no radio");
        if (ops->ot_stop() != ESP_OK) {
            mode_fsm_set_fault(fsm); // still on the target, which the stored modes do not name
            return MODE_FSM_ERR_START_FAILED;
        }
    } else {
        snprintf(err, cap, "OpenThread did not start on %s", to);
    }
    return start_back(fsm, prev, from, to, err, cap);
}

void mode_fsm_on_channel_gone(mode_fsm_t *fsm)
{
    if (fsm->radio_job) {
        return; // the job took the radio: it starts OpenThread again itself
    }
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
    } else if (!fsm->radio_job) {
        // OpenThread is gone: NO_RADIO again, also when its task parked itself into RADIO_FAULT during the drain.
        set_mode(fsm, BR_MODE_NO_RADIO);
    }
}

void mode_fsm_on_channel_back(mode_fsm_t *fsm, const char *id)
{
    char err[64];

    if (fsm->radio_job) {
        return;
    }
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
