#include "mode_fsm.h"

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
    }
    return "?";
}

static void set_mode(mode_fsm_t *fsm, br_core_mode_t mode)
{
    atomic_store(&fsm->mode, mode);
    ESP_LOGI(TAG, "mode %s", mode_name(mode));
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
    const bool changed = atomic_compare_exchange_strong(&fsm->mode, &expected, BR_MODE_BR_RUNNING);

    if (changed) {
        ESP_LOGI(TAG, "mode %s", mode_name(BR_MODE_BR_RUNNING));
    }
    return changed;
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

    ESP_LOGI(TAG, "mode %s", mode_name(BR_MODE_RADIO_FAULT));
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
            ESP_LOGI(TAG, "mode %s", mode_name(fsm->radio_job_prev));
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

void mode_fsm_on_rcp_failure(mode_fsm_t *fsm)
{
    const mode_fsm_ops_t *ops = fsm->ops;
    const br_core_mode_t mode = mode_fsm_get_mode(fsm);

    if (mode == BR_MODE_STARTING) {
        ops->reset_rcp();
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

    if (ops->reset_rcp() != ESP_OK) {
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

void mode_fsm_installed_after_push(bool probe_ok, const char *probed_version, bool *known, char *out, size_t cap)
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

bool mode_fsm_rom_failure_is_fault(const rcp_sync_probe_result_t *probe)
{
    return probe->rom_recovery_failed && !probe->updater_reachable;
}

bool mode_fsm_next_recovery_failed(bool previous, bool attempted, bool succeeded)
{
    return attempted ? !succeeded : previous;
}

esp_err_t mode_fsm_radio_job_result(bool base_only, esp_err_t job_err, esp_err_t restart_err)
{
    if (base_only || job_err != ESP_OK) {
        return job_err;
    }
    return restart_err;
}
