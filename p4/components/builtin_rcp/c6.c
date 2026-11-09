// This board's built-in RCP: the ESP32-C6 on the ESP32-P4 board, with rcp_sync as its flashing engine (over SPI3 and
// the INT pin; the radio mux's SPI2 transport carries spinel). The probe of the installed image, the push, the ROM
// recovery over the maintenance jumper, the check for the factory ESP-Hosted firmware, and the test build's overrides.
#include <string.h>

#include "board.h"
#include "builtin_rcp.h"
#include "builtin_rcp_core.h"
#include "builtin_rcp_priv.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "builtin_rcp";

// Fault reasons of the boot steps (the board shows them in RADIO_FAULT while Thread runs on the C6).
#define FAULT_NO_RCP "no rcp firmware"
#define FAULT_ROM_RECOVERY "rom recovery failed"
#define FAULT_C6_HELD "rcp incompatible" // the test build's c6_hold: the C6 stays in reset, as an RCP that never answers
#define FAULT_RESET "resetting the RCP failed"

// The boot-time probe, best effort. The test build can force the result of the first probe of a boot; the probe
// after a firmware push (which restarts the radio through here) is always real.
static esp_err_t run_probe(rcp_sync_probe_result_t *probe)
{
#ifdef OTBR_TEST_BUILD
    static bool override_used;
    const builtin_rcp_test_probe_t forced = override_used ? BUILTIN_RCP_TEST_PROBE_REAL :
                                            builtin_rcp_test_probe_override();

    override_used = true;
    if (forced != BUILTIN_RCP_TEST_PROBE_REAL) {
        ESP_LOGW(TAG, "test build: probe forced to %s", forced == BUILTIN_RCP_TEST_PROBE_EMPTY ? "empty" : "fail");
        memset(probe, 0, sizeof(*probe));
        probe->updater_reachable = forced == BUILTIN_RCP_TEST_PROBE_EMPTY;
        strlcpy(probe->last_error, "forced by the test build", sizeof(probe->last_error));
        return ESP_OK;
    }
#endif
    return rcp_sync_probe(probe);
}

static bool c6_present(radio_mux_fault_kind_t *kind, char *reason, size_t cap)
{
    rcp_sync_probe_result_t r;

    rcp_sync_get_last_probe(&r);
    return builtin_rcp_present(&r, kind, reason, cap);
}

// The C6's own boot steps (spec 5.3 steps 3 and 4): probe the installed RCP over the updater (rcp_sync, SPI3), the ROM
// recovery it may run, then reset the C6 into its RCP. Nothing is pushed. The status follows the probe.
static esp_err_t c6_boot_steps(radio_mux_fault_kind_t *kind, char *reason, size_t cap)
{
    rcp_sync_probe_result_t probe;
    esp_err_t err = run_probe(&probe);
    const char *fault = NULL;
    bool held = false;

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "RCP probe could not run: %s", esp_err_to_name(err));
        memset(&probe, 0, sizeof(probe));
    }
    builtin_rcp_note_probe(&probe);
    if (builtin_rcp_rom_failure_is_fault(&probe)) {
        // Spec 7: the updater was silent and the recovery failed; no further automatic attempt this boot.
        ESP_LOGE(TAG, "the ROM recovery of the C6 base image failed");
        fault = FAULT_ROM_RECOVERY;
    } else if (probe.esp_hosted) {
        // A factory C6: OpenThread cannot run on it. The device page asks for c6_installer.bin.
        ESP_LOGE(TAG, "the C6 runs its factory ESP-Hosted firmware: upload c6_installer.bin, then c6_rcp.bin");
        fault = RCP_SYNC_ESP_HOSTED_ERROR;
    } else if (probe.updater_reachable && !probe.installed_valid) {
        ESP_LOGE(TAG, "the C6 holds no RCP firmware");
        fault = FAULT_NO_RCP;
    }
    if (probe.rom_recovery_failed && !fault) {
        // The updater answered (base version mismatch): an RCP may be installed, so the boot goes on with it.
        ESP_LOGE(TAG, "the ROM recovery of the C6 base image failed; continuing with the installed RCP (see status "
                 "jumper.recovery_failed)");
    }
    if (!fault && !probe.updater_reachable) {
        ESP_LOGW(TAG, "RCP probe failed (%s), trying the installed firmware", probe.last_error);
    } else if (!fault) {
        ESP_LOGI(TAG, "installed RCP firmware %s", probe.installed_version);
    }
#ifdef OTBR_TEST_BUILD
    if (!fault && builtin_rcp_test_c6_hold()) {
        ESP_LOGW(TAG, "test build: holding the C6 in reset");
        gpio_set_level(P4_PIN_C6_RESET, 0);
        fault = FAULT_C6_HELD;
        held = true;
    }
#endif
    if (!fault && rcp_sync_boot_rcp() != ESP_OK) {
        fault = FAULT_RESET;
    }
    *kind = fault ? RADIO_MUX_FAULT_FIRMWARE : RADIO_MUX_FAULT_NONE;
    strlcpy(reason, fault ? fault : "", cap);
    return held ? ESP_ERR_INVALID_STATE : fault ? ESP_ERR_NOT_FOUND : ESP_OK;
}

static esp_err_t c6_push(const void *source, char *detail, size_t cap, bool *base_only)
{
    return rcp_sync_push_firmware(source, detail, cap, base_only);
}

// A manual ROM recovery of the base image. A failed one is followed by the boot steps' probe, which may run the one
// automatic recovery of this boot (the probe sees the same trigger): a second attempt after a manual one is intended,
// not a loop.
static esp_err_t c6_rom_recover(char *detail, size_t cap)
{
    const esp_err_t err = rcp_sync_rom_recover();

    // Refusals (no jumper, busy, no image) are no attempt; anything else is, and decides the status flag.
    builtin_rcp_note_recovery(err != ESP_ERR_NOT_SUPPORTED && err != ESP_ERR_INVALID_STATE, err == ESP_OK);
    if (err != ESP_OK && detail && cap > 0) {
        strlcpy(detail, err == ESP_ERR_NOT_SUPPORTED ? "no maintenance jumper" :
                "ROM recovery of the C6 base image failed", cap);
    }
    return err;
}

const builtin_rcp_board_t builtin_rcp_board = {
    .chip = "esp32c6",
    .label = "ESP32-C6 (built-in)",
    .present = c6_present,
    .boot_steps = c6_boot_steps,
    .push = c6_push,
    .rom_recover = c6_rom_recover,
};

#ifdef OTBR_TEST_BUILD
// Test-only hooks, persisted under the keys br_core used, so a board's test state carries over.
#define NAMESPACE "otbr_dev"
#define KEY_C6_HOLD "test_c6_hold"
#define KEY_PROBE "test_probe"

static uint8_t test_get(const char *key, uint8_t fallback)
{
    nvs_handle_t handle;
    uint8_t value = fallback;

    if (nvs_open(NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return fallback;
    }
    nvs_get_u8(handle, key, &value);
    nvs_close(handle);
    return value;
}

static esp_err_t test_set(const char *key, uint8_t value)
{
    nvs_handle_t handle;

    ESP_RETURN_ON_ERROR(nvs_open(NAMESPACE, NVS_READWRITE, &handle), TAG, "nvs_open");
    esp_err_t err = nvs_set_u8(handle, key, value);

    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

bool builtin_rcp_test_c6_hold(void)
{
    return test_get(KEY_C6_HOLD, 0) != 0;
}

esp_err_t builtin_rcp_test_set_c6_hold(bool hold)
{
    return test_set(KEY_C6_HOLD, hold ? 1 : 0);
}

builtin_rcp_test_probe_t builtin_rcp_test_probe_override(void)
{
    const uint8_t value = test_get(KEY_PROBE, BUILTIN_RCP_TEST_PROBE_REAL);

    return value <= BUILTIN_RCP_TEST_PROBE_FAIL ? (builtin_rcp_test_probe_t)value : BUILTIN_RCP_TEST_PROBE_REAL;
}

esp_err_t builtin_rcp_test_set_probe_override(builtin_rcp_test_probe_t mode)
{
    return test_set(KEY_PROBE, (uint8_t)mode);
}
#endif // OTBR_TEST_BUILD
