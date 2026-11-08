// The built-in RCP, chip-independent: its registration with the radio mux and its status. The board's RCP (its probe,
// push, ROM recovery and checks) is builtin_rcp_board, in its own file.
#include "builtin_rcp.h"

#include <string.h>

#include "builtin_rcp_core.h"
#include "builtin_rcp_priv.h"
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

esp_err_t builtin_rcp_register(void)
{
    const builtin_rcp_board_t *b = &builtin_rcp_board;

    s_desc = (radio_mux_builtin_t) {
        .id = RADIO_MUX_C6_ID, .label = b->label, .chip = b->chip, .present = b->present, .boot_steps = b->boot_steps,
        .push = b->push, .rom_recover = b->rom_recover,
    };
    ESP_LOGI(TAG, "%s: %s", s_desc.id, s_desc.label);
    return radio_mux_set_builtin(&s_desc);
}
