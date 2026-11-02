// Test-only console commands, compiled only when OTBR_TEST_BUILD is defined (tools/build_all.sh --test).
#ifdef OTBR_TEST_BUILD

#include "test_cmds.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_br_http_ota.h"
#include "esp_console.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_spiffs.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "nvs.h"

#include "board.h"
#include "br_core.h"
#include "device_api.h"
#include "c6_board.h"
#include "driver/gpio.h"
#include "rcp_sync.h"
#include "storage.h"

static const char *TAG = "test_cmds";

#define CAT_MAX_BYTES 128

// fsinfo <label>: total and used bytes of a mounted SPIFFS, by partition label.
static int cmd_fsinfo(int argc, char **argv)
{
    if (argc != 2) {
        printf("usage: fsinfo <mount-path>\n");
        return 1;
    }
    const char *label = NULL;
    if (strcmp(argv[1], "/web") == 0) {
        label = storage_running_slot() == 0 ? "web_0" : "web_1";
    }
    size_t total = 0;
    size_t used = 0;
    if (!label || esp_spiffs_info(label, &total, &used) != ESP_OK) {
        printf("fsinfo: %s not mounted\n", argv[1]);
        return 1;
    }
    printf("fsinfo: %s total=%u used=%u\n", argv[1], (unsigned)total, (unsigned)used);
    return 0;
}

// cat <path>: first bytes of a file.
static int cmd_cat(int argc, char **argv)
{
    if (argc != 2) {
        printf("usage: cat <path>\n");
        return 1;
    }
    FILE *file = fopen(argv[1], "r");
    if (!file) {
        printf("cat: %s: not found\n", argv[1]);
        return 1;
    }
    char buffer[CAT_MAX_BYTES + 1];
    size_t length = fread(buffer, 1, CAT_MAX_BYTES, file);
    fclose(file);
    buffer[length] = '\0';
    printf("cat: %s: %s\n", argv[1], buffer);
    return 0;
}

// boot_slot <0|1>: commit ota_N as boot partition and restart.
static int cmd_boot_slot(int argc, char **argv)
{
    const int slot = argc == 2 ? atoi(argv[1]) : -1;
    if (slot != 0 && slot != 1) {
        printf("usage: boot_slot <0|1>\n");
        return 1;
    }
    const esp_partition_t *partition = esp_partition_find_first(
                                           ESP_PARTITION_TYPE_APP, slot == 0 ? ESP_PARTITION_SUBTYPE_APP_OTA_0 : ESP_PARTITION_SUBTYPE_APP_OTA_1, NULL);
    if (!partition || esp_ota_set_boot_partition(partition) != ESP_OK) {
        printf("boot_slot: setting ota_%d failed\n", slot);
        return 1;
    }
    printf("boot_slot: ota_%d\n", slot);
    fflush(stdout);
    esp_restart();
    return 0;
}

#define DEV_NAMESPACE "otbr_dev"
#define KEEP_PENDING_KEY "keep_pending"

// keep_pending [0]: the next boot leaves its image pending-verify (consumed once, see test_cmds_start); 0 clears it.
static int cmd_keep_pending(int argc, char **argv)
{
    const bool clear = argc == 2 && atoi(argv[1]) == 0;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(DEV_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = clear ? nvs_erase_key(handle, KEEP_PENDING_KEY) : nvs_set_u8(handle, KEEP_PENDING_KEY, 1);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
        if (err == ESP_OK) {
            err = nvs_commit(handle);
        }
        nvs_close(handle);
    }
    printf("keep_pending: %s\n", err == ESP_OK ? (clear ? "cleared" : "set") : esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

// Read and erase the keep_pending flag.
static bool consume_keep_pending(void)
{
    nvs_handle_t handle;
    uint8_t value = 0;
    if (nvs_open(DEV_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        return false;
    }
    if (nvs_get_u8(handle, KEEP_PENDING_KEY, &value) == ESP_OK) {
        nvs_erase_key(handle, KEEP_PENDING_KEY);
        nvs_commit(handle);
    }
    nvs_close(handle);
    return value != 0;
}

// ota_state: running slot and its rollback state.
static int cmd_ota_state(int argc, char **argv)
{
    static const char *const names[] = {"NEW", "PENDING_VERIFY", "VALID", "INVALID", "ABORTED", "UNDEFINED"};
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    esp_ota_get_state_partition(esp_ota_get_running_partition(), &state);
    printf("ota_state: running=%d state=%s\n", storage_running_slot(),
           state < sizeof(names) / sizeof(names[0]) ? names[state] : "?");
    return 0;
}

// crash: abort, as a panic would.
static int cmd_crash(int argc, char **argv)
{
    printf("crash: aborting\n");
    fflush(stdout);
    abort();
    return 0;
}

// c6_hold <0|1>: from the next boot on, hold the C6 in reset instead of booting the RCP.
static int cmd_c6_hold(int argc, char **argv)
{
    if (argc != 2) {
        printf("usage: c6_hold <0|1>\n");
        return 1;
    }
    esp_err_t err = br_core_test_set_c6_hold(atoi(argv[1]) != 0);
    printf("c6_hold: %s\n", err == ESP_OK ? "ok" : esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

// c6_jumper_ignore <0|1>: the probe and the push treat the maintenance jumper as absent (persisted; the first install
// through ESP-Hosted can then be tested with the jumper still fitted).
static int cmd_c6_jumper_ignore(int argc, char **argv)
{
    if (argc != 2) {
        printf("usage: c6_jumper_ignore <0|1>\n");
        return 1;
    }
    esp_err_t err = rcp_sync_test_set_jumper_ignore(atoi(argv[1]) != 0);
    printf("c6_jumper_ignore: %s\n", err == ESP_OK ? "ok" : esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

// br_status: br_core mode, installed RCP version, dataset and DHCPv6-PD state.
static int cmd_br_status(int argc, char **argv)
{
    static const char *const modes[] = {"STARTING", "BR_RUNNING", "SNIFFER", "RADIO_FAULT"};
    br_core_status_t status;
    br_core_get_status(&status);
    printf("br_status: mode=%s installed=%s known=%d compatible=%d dataset=%d channel=%u pd=%d prefix=%s fault=%s\n",
           modes[status.mode], status.rcp_installed, status.rcp_installed_known, status.rcp_compatible, status.has_dataset,
           status.channel, status.pd_state, status.pd_prefix, status.fault_reason);
    return 0;
}

// c6_inject_reset_at <offset>: during the next radio firmware push, reset the C6 once the image is read up to
// <offset> bytes (0 = off).
static int cmd_c6_inject_reset_at(int argc, char **argv)
{
    if (argc != 2) {
        printf("usage: c6_inject_reset_at <offset>\n");
        return 1;
    }
    device_api_test_inject_c6_reset_at((uint32_t)atoi(argv[1]));
    printf("c6_inject_reset_at: ok\n");
    return 0;
}

// rcp_probe_override <real|empty|fail>: from the next boot on, force the boot-time probe result.
static int cmd_rcp_probe_override(int argc, char **argv)
{
    static const char *const names[] = {"real", "empty", "fail"};
    int mode = -1;
    for (int i = 0; argc == 2 && i < 3; i++) {
        if (strcmp(argv[1], names[i]) == 0) {
            mode = i;
        }
    }
    if (mode < 0) {
        printf("usage: rcp_probe_override <real|empty|fail>\n");
        return 1;
    }
    esp_err_t err = br_core_test_set_probe_override((br_core_test_probe_t)mode);
    printf("rcp_probe_override: %s\n", err == ESP_OK ? "ok" : esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

// rcp_kill: hold the C6 in reset now (and for every reset until `c6_hold 0` and a reboot): a runtime RCP failure.
static int cmd_rcp_kill(int argc, char **argv)
{
    esp_err_t err = br_core_test_set_c6_hold(true);
    gpio_set_level(P4_PIN_C6_RESET, 0);
    printf("rcp_kill: %s\n", esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

// rcp_glitch: reset the C6 for 50 ms without holding it: an RCP crash that recovers by itself.
static int cmd_rcp_glitch(int argc, char **argv)
{
    gpio_set_level(P4_PIN_C6_RESET, 0);
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(P4_PIN_C6_RESET, 1);
    printf("rcp_glitch: ok\n");
    return 0;
}

#define OTA_TASK_STACK 8192
#define OTA_HTTP_TIMEOUT_MS 10000
#define OTA_REBOOT_DELAY_MS 1000

typedef struct {
    char url[192];
    SemaphoreHandle_t done; // given when the update finished; NULL for the background variant
} ota_job_t;

// Runs esp_br_http_ota (bundle by URL) on its own task; on success the board restarts into the new pair.
static void ota_task(void *arg)
{
    ota_job_t *job = arg;
    esp_http_client_config_t config = {.url = job->url, .timeout_ms = OTA_HTTP_TIMEOUT_MS};
    esp_err_t err = esp_br_http_ota(&config);
    printf("ota_url: %s\n", esp_err_to_name(err));
    fflush(stdout);
    SemaphoreHandle_t done = job->done;
    free(job);
    if (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(OTA_REBOOT_DELAY_MS));
        esp_restart();
    }
    if (done) {
        xSemaphoreGive(done);
    }
    vTaskDelete(NULL);
}

static int start_ota(const char *url, bool wait)
{
    ota_job_t *job = calloc(1, sizeof(*job));
    if (!job || strlen(url) >= sizeof(job->url)) {
        free(job);
        printf("usage: <ota_url|ota_url_bg> <url> (url too long or out of memory)\n");
        return 1;
    }
    strcpy(job->url, url);
    if (wait) {
        job->done = xSemaphoreCreateBinary();
    }
    SemaphoreHandle_t done = job->done;
    if (xTaskCreate(ota_task, "ota_job", OTA_TASK_STACK, job, 5, NULL) != pdPASS) {
        if (done) {
            vSemaphoreDelete(done);
        }
        free(job);
        printf("ota_url: ESP_ERR_NO_MEM\n");
        return 1;
    }
    if (done) {
        xSemaphoreTake(done, portMAX_DELAY);
        vSemaphoreDelete(done);
    }
    return 0;
}

// ota_url <url>: esp_br_http_ota of a bundle; prints "ota_url: <esp_err>" and restarts on success.
static int cmd_ota_url(int argc, char **argv)
{
    if (argc != 2) {
        printf("usage: ota_url <url>\n");
        return 1;
    }
    return start_ota(argv[1], true);
}

// ota_url_bg <url>: as ota_url, but returns at once (the console stays usable during the update).
static int cmd_ota_url_bg(int argc, char **argv)
{
    if (argc != 2) {
        printf("usage: ota_url_bg <url>\n");
        return 1;
    }
    return start_ota(argv[1], false);
}

// ota_probe: start an update and give it up at once; shows whether a second concurrent update is refused.
static int cmd_ota_probe(int argc, char **argv)
{
    esp_br_ota_handle_t handle = NULL;
    esp_err_t err = esp_br_ota_begin(&handle);
    if (err == ESP_OK) {
        esp_br_ota_abort(handle);
    }
    printf("ota_probe: %s (0x%x)\n", esp_err_to_name(err), (unsigned)err);
    return err == ESP_OK ? 0 : 1;
}

// ota_commit_delay <ms>: wait this long between the verified bundle and esp_ota_set_boot_partition (reset window).
static int cmd_ota_commit_delay(int argc, char **argv)
{
    esp_br_ota_test_set_commit_delay_ms(argc == 2 ? (uint32_t)atoi(argv[1]) : 0);
    printf("ota_commit_delay: ok\n");
    return 0;
}

// ota_boot: slot of the boot partition (what the next reset runs) next to the running one.
static int cmd_ota_boot(int argc, char **argv)
{
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    printf("ota_boot: boot=%d running=%d\n", boot ? (int)(boot->subtype - ESP_PARTITION_SUBTYPE_APP_OTA_0) : -1,
           storage_running_slot());
    return 0;
}

#define ERASE_TASK_STACK 8192

typedef struct {
    uint32_t offset;
    uint32_t size;
    esp_err_t result;
    SemaphoreHandle_t done;
} erase_job_t;

// Own task: the flasher needs more stack than the console task has.
static void erase_task(void *arg)
{
    erase_job_t *job = arg;
    job->result = rcp_sync_test_c6_erase(job->offset, job->size);
    xSemaphoreGive(job->done);
    vTaskDelete(NULL);
}

// c6_erase <bootloader|ptable|otadata|test|ota0|ota1>: erase one C6 flash region over the maintenance jumper (ROM download), then
// reset the C6. Does not stop OpenThread: reboot the P4 or POST /device/radio/recover afterwards.
static int cmd_c6_erase(int argc, char **argv)
{
    static const struct {
        const char *name;
        uint32_t offset;
        uint32_t size;
    } regions[] = {
        {"bootloader", C6_FLASH_BOOTLOADER_OFFSET, C6_FLASH_PTABLE_OFFSET - C6_FLASH_BOOTLOADER_OFFSET},
        {"ptable", C6_FLASH_PTABLE_OFFSET, C6_FLASH_NVS_OFFSET - C6_FLASH_PTABLE_OFFSET},
        {"otadata", C6_FLASH_OTADATA_OFFSET, C6_FLASH_OTADATA_SIZE},
        {"test", C6_FLASH_TEST_OFFSET, C6_FLASH_TEST_SIZE},
        // First sector of an RCP slot (the image magic): that slot no longer boots. A whole slot is too much for the
        // ROM loader's erase and verify (ESP_FAIL).
        {"ota0", C6_FLASH_OTA0_OFFSET, 0x1000},
        {"ota1", C6_FLASH_OTA0_OFFSET + C6_OTA_SLOT_SIZE, 0x1000},
    };
    for (size_t i = 0; argc == 2 && i < sizeof(regions) / sizeof(regions[0]); i++) {
        if (strcmp(argv[1], regions[i].name) != 0) {
            continue;
        }
        erase_job_t job = {.offset = regions[i].offset, .size = regions[i].size, .done = xSemaphoreCreateBinary()};
        if (!job.done || xTaskCreate(erase_task, "c6_erase", ERASE_TASK_STACK, &job, 5, NULL) != pdPASS) {
            printf("c6_erase: ESP_ERR_NO_MEM\n");
            return 1;
        }
        xSemaphoreTake(job.done, portMAX_DELAY);
        vSemaphoreDelete(job.done);
        printf("c6_erase: %s\n", job.result == ESP_OK ? "ok" : esp_err_to_name(job.result));
        return job.result == ESP_OK ? 0 : 1;
    }
    printf("usage: c6_erase <bootloader|ptable|otadata|test|ota0|ota1>\n");
    return 1;
}

#define BUSY_TASK_STACK 2048
#define BUSY_MAX_SECONDS 3 // the idle task of that core is starved meanwhile; its watchdog fires after 5 s
#define BUSY_PRIORITY 1    // application minimum: shares the core with the diag sampler instead of starving it

static void busy_task(void *arg)
{
    const int64_t until_us = esp_timer_get_time() + (int64_t)(intptr_t)arg * 1000000;

    while (esp_timer_get_time() < until_us) {
        // spin
    }
    printf("busy: done\n");
    vTaskDelete(NULL);
}

// busy <core> <seconds>: spin a task pinned to <core> for 1..3 s and return at once (load for /device/diag tests).
static int cmd_busy(int argc, char **argv)
{
    const long core = argc == 3 ? strtol(argv[1], NULL, 10) : -1;
    const long seconds = argc == 3 ? strtol(argv[2], NULL, 10) : 0;

    if ((core != 0 && core != 1) || seconds < 1 || seconds > BUSY_MAX_SECONDS) {
        printf("usage: busy <0|1> <1..%d>\n", BUSY_MAX_SECONDS);
        return 1;
    }
    if (xTaskCreatePinnedToCore(busy_task, "busy", BUSY_TASK_STACK, (void *)(intptr_t)seconds, BUSY_PRIORITY, NULL, (BaseType_t)core) != pdPASS) {
        printf("busy: ESP_ERR_NO_MEM\n");
        return 1;
    }
    printf("busy: started core=%ld seconds=%ld\n", core, seconds);
    return 0;
}

static esp_err_t register_command(const char *name, const char *help, esp_console_cmd_func_t func)
{
    const esp_console_cmd_t command = {.command = name, .help = help, .func = func};
    return esp_console_cmd_register(&command);
}

esp_err_t test_cmds_register(void)
{
    // Rollback is enabled: without this, a reboot while running a freshly selected slot (boot_slot) would fall
    // back to the previous one. Test builds confirm every image so tests can reboot inside either slot, except
    // after keep_pending: that boot stays pending-verify, so the production path (validity.c) can be tested.
    if (consume_keep_pending()) {
        ESP_LOGW(TAG, "leaving this image pending-verify (keep_pending)");
    } else {
        esp_err_t valid = esp_ota_mark_app_valid_cancel_rollback();
        if (valid != ESP_OK && valid != ESP_ERR_NOT_SUPPORTED) {
            ESP_LOGW(TAG, "marking app valid: %s", esp_err_to_name(valid));
        }
    }
    ESP_RETURN_ON_ERROR(register_command("fsinfo", "total/used bytes of /web", cmd_fsinfo), TAG, "fsinfo");
    ESP_RETURN_ON_ERROR(register_command("cat", "print the start of a file", cmd_cat), TAG, "cat");
    ESP_RETURN_ON_ERROR(register_command("boot_slot", "commit ota_0 or ota_1 and restart", cmd_boot_slot), TAG, "boot_slot");
    ESP_RETURN_ON_ERROR(register_command("keep_pending", "leave the next boot's image pending-verify", cmd_keep_pending), TAG, "keep_pending");
    ESP_RETURN_ON_ERROR(register_command("ota_state", "running slot and rollback state", cmd_ota_state), TAG, "ota_state");
    ESP_RETURN_ON_ERROR(register_command("crash", "abort (panic)", cmd_crash), TAG, "crash");
    ESP_RETURN_ON_ERROR(register_command("c6_hold", "hold the C6 in reset from the next boot (0 clears)", cmd_c6_hold), TAG, "c6_hold");
    ESP_RETURN_ON_ERROR(register_command("c6_jumper_ignore", "treat the maintenance jumper as absent (0 clears)", cmd_c6_jumper_ignore),
                        TAG, "c6_jumper_ignore");
    ESP_RETURN_ON_ERROR(register_command("busy", "spin a task on a core for 1..3 s", cmd_busy), TAG, "busy");
    ESP_RETURN_ON_ERROR(register_command("br_status", "br_core mode, dataset and DHCPv6-PD state", cmd_br_status), TAG, "br_status");
    ESP_RETURN_ON_ERROR(register_command("rcp_probe_override", "force the next boot's probe: real, empty or fail", cmd_rcp_probe_override), TAG, "rcp_probe_override");
    ESP_RETURN_ON_ERROR(register_command("c6_inject_reset_at", "c6_inject_reset_at <offset>: reset the C6 mid-push", cmd_c6_inject_reset_at), TAG, "c6_inject_reset_at");
    ESP_RETURN_ON_ERROR(register_command("rcp_kill", "hold the C6 in reset now (runtime RCP failure)", cmd_rcp_kill), TAG, "rcp_kill");
    ESP_RETURN_ON_ERROR(register_command("rcp_glitch", "reset the C6 briefly, no hold (self-recovering RCP crash)", cmd_rcp_glitch), TAG, "rcp_glitch");
    ESP_RETURN_ON_ERROR(register_command("ota_url", "ota_url <url>: bundle OTA by URL, restarts on success", cmd_ota_url), TAG, "ota_url");
    ESP_RETURN_ON_ERROR(register_command("ota_url_bg", "ota_url_bg <url>: as ota_url, returns at once", cmd_ota_url_bg), TAG, "ota_url_bg");
    ESP_RETURN_ON_ERROR(register_command("ota_probe", "begin and abort an update: shows if one is running", cmd_ota_probe), TAG, "ota_probe");
    ESP_RETURN_ON_ERROR(register_command("ota_commit_delay", "ota_commit_delay <ms>: wait before the boot partition is set", cmd_ota_commit_delay), TAG, "ota_commit_delay");
    ESP_RETURN_ON_ERROR(register_command("ota_boot", "boot and running slot", cmd_ota_boot), TAG, "ota_boot");
    ESP_RETURN_ON_ERROR(register_command("c6_erase", "c6_erase <bootloader|ptable|otadata|test|ota0|ota1>: erase a C6 flash region over the jumper", cmd_c6_erase), TAG, "c6_erase");
    return br_core_test_ot_command_register();
}

#endif // OTBR_TEST_BUILD
