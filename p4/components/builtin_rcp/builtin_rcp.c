// The built-in RCP, chip-independent: its registration with the radio mux and its status. The board's RCP (its probe,
// push, ROM recovery and checks) is builtin_rcp_board, in its own file.
#include "builtin_rcp.h"

#include <string.h>

#include "builtin_rcp_core.h"
#include "builtin_rcp_priv.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "builtin_rcp";

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static builtin_rcp_status_t s_status; // under s_lock; `jumper` is read live

static radio_mux_builtin_t s_desc;

void builtin_rcp_note_probe(const rcp_sync_probe_result_t *probe)
{
    taskENTER_CRITICAL(&s_lock);
    s_status.installed_known = probe->updater_reachable;
    strlcpy(s_status.installed, probe->installed_version, sizeof(s_status.installed));
    s_status.rom_recovery_failed = builtin_rcp_next_recovery_failed(s_status.rom_recovery_failed,
                                                                    probe->rom_recovery_failed || probe->rom_recovered,
                                                                    probe->rom_recovered);
    taskEXIT_CRITICAL(&s_lock);
}

void builtin_rcp_note_recovery(bool attempted, bool succeeded)
{
    taskENTER_CRITICAL(&s_lock);
    s_status.rom_recovery_failed = builtin_rcp_next_recovery_failed(s_status.rom_recovery_failed, attempted,
                                                                    succeeded);
    taskEXIT_CRITICAL(&s_lock);
}

void builtin_rcp_get_status(builtin_rcp_status_t *out)
{
    taskENTER_CRITICAL(&s_lock);
    *out = s_status;
    taskEXIT_CRITICAL(&s_lock);
    out->jumper = rcp_sync_jumper_present();
}

// One maintenance of the built-in RCP through its channel's reinit; the result as builtin_rcp_job_result decides it.
static esp_err_t maintain(radio_mux_work_t work, const rcp_fw_source_t *source, char *detail, size_t cap)
{
    const char *what = work == RADIO_MUX_WORK_PUSH ? "RCP firmware push" : "ROM recovery";
    radio_mux_maint_t done = {0};
    const esp_err_t err = radio_mux_maintain(RADIO_MUX_BUILTIN_ID, work, source, detail, cap, &done);

    if (!done.ran) {
        if (err == RADIO_MUX_ERR_PEER_CONNECTED && detail && cap > 0) {
            strlcpy(detail, "c6_busy: rcp-proxy on the C6 has a peer", cap); // the reply's detail as before
        }
        ESP_LOGW(TAG, "%s refused: %s (%s)", what, esp_err_to_name(err), detail ? detail : "");
        return err; // nothing was touched
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s failed: %s", what, esp_err_to_name(err));
    }
    const esp_err_t result = builtin_rcp_job_result(work == RADIO_MUX_WORK_ROM_RECOVERY || done.base_only, err,
                                                    done.settle);

    if (result == RCP_SYNC_ERR_RESTART) {
        char why[BUILTIN_RCP_DETAIL_MAX];

        builtin_rcp_restart_detail(why, sizeof(why), &done);
        ESP_LOGW(TAG, "%s: %s", what, why);
        if (detail && cap > 0) {
            strlcpy(detail, why, cap);
        }
    }
    return result;
}

esp_err_t builtin_rcp_push_firmware(const rcp_fw_source_t *source, char *detail, size_t cap)
{
    if (detail && cap > 0) {
        detail[0] = '\0';
    }
    ESP_RETURN_ON_FALSE(source && source->read, ESP_ERR_INVALID_ARG, TAG, "no firmware source");
    return maintain(RADIO_MUX_WORK_PUSH, source, detail, cap);
}

esp_err_t builtin_rcp_rom_recover(char *detail, size_t cap)
{
    if (detail && cap > 0) {
        detail[0] = '\0';
    }
    // Before anything is asked: without the jumper there is nothing to recover with.
    ESP_RETURN_ON_FALSE(rcp_sync_jumper_present(), ESP_ERR_NOT_SUPPORTED, TAG, "no maintenance jumper");
    return maintain(RADIO_MUX_WORK_ROM_RECOVERY, NULL, detail, cap);
}

esp_err_t builtin_rcp_register(void)
{
    const builtin_rcp_board_t *b = &builtin_rcp_board;

    s_desc = (radio_mux_builtin_t) {
        .id = RADIO_MUX_BUILTIN_ID, .label = b->label, .chip = b->chip, .present = b->present, .boot_steps = b->boot_steps,
        .push = b->push, .rom_recover = b->rom_recover,
    };
    ESP_LOGI(TAG, "%s: %s", s_desc.id, s_desc.label);
    return radio_mux_set_builtin(&s_desc);
}
