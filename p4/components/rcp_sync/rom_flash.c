// ROM recovery over the maintenance jumper (spec 5.10): esp-serial-flasher on JUMPER_TX/JUMPER_RX, JUMPER_BOOT as BOOT,
// GPIO54 as reset. The decisions (what is written, verified, retried) are in rom_plan.c; this file is the flasher glue.
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include "driver/gpio.h"
#include "esp32_port.h"
#include "esp_check.h"
#include "esp_loader.h"
#include "esp_log.h"
#include "esp_rom_md5.h"
#include "board.h"
#include "base_image.h"
#include "jumper.h"
#include "rcp_sync.h"
#include "rom_plan.h"

static const char *TAG = "rcp_sync";

#define CONNECT_BAUD      115200u
#define BLOCK_SIZE        4096u
#define MD5_LEN           16
#define ERASED_BYTE       0xff

static struct {
    bool port_up;   // loader_port_esp32_init succeeded: its UART is to be deleted, the C6 can be reset
    uint8_t *block;
} s_fl;
static atomic_bool s_busy;

static void release_pin(int pin)
{
    gpio_reset_pin(pin); // input and output off, pull-up on
    gpio_set_pull_mode(pin, GPIO_FLOATING);
}

// Fresh ROM download session at `baud` (rom_plan.h). The first call sets up the buffer, the UART and the pins; later ones (a
// retry after an error) switch the UART back to the ROM's default rate and connect again, which resets the C6 into its loader.
static esp_err_t op_connect(uint32_t baud)
{
    const loader_esp32_config_t config = {
        .baud_rate = CONNECT_BAUD,
        .uart_port = JUMPER_UART_PORT,
        .uart_rx_pin = P4_PIN_JUMPER_RX,
        .uart_tx_pin = P4_PIN_JUMPER_TX,
        .reset_trigger_pin = P4_PIN_C6_RESET,
        .gpio0_trigger_pin = P4_PIN_JUMPER_BOOT,
    };
    esp_loader_connect_args_t args = ESP_LOADER_CONNECT_DEFAULT();

    if (!s_fl.port_up) {
        s_fl.block = malloc(BLOCK_SIZE);
        ESP_RETURN_ON_FALSE(s_fl.block, ESP_ERR_NO_MEM, TAG, "no memory for the flash buffer");
        ESP_RETURN_ON_FALSE(loader_port_esp32_init(&config) == ESP_LOADER_SUCCESS, ESP_FAIL, TAG, "flasher port init failed");
        s_fl.port_up = true;
    } else {
        ESP_LOGW(TAG, "reconnecting to the ROM loader");
        ESP_RETURN_ON_FALSE(loader_port_change_transmission_rate(CONNECT_BAUD) == ESP_LOADER_SUCCESS, ESP_FAIL, TAG,
                            "switching the UART back to %u baud failed", (unsigned)CONNECT_BAUD);
    }
    esp_loader_error_t err = esp_loader_connect(&args);

    ESP_RETURN_ON_FALSE(err == ESP_LOADER_SUCCESS, ESP_FAIL, TAG, "no ROM download mode on the C6 (%d)", err);
    ESP_RETURN_ON_FALSE(esp_loader_get_target() == ESP32C6_CHIP, ESP_ERR_NOT_SUPPORTED, TAG, "the target is not an ESP32-C6 (%d)",
                        (int)esp_loader_get_target());
    if (baud == CONNECT_BAUD) {
        return ESP_OK;
    }
    if (esp_loader_change_transmission_rate(baud) == ESP_LOADER_SUCCESS) {
        // The C6 has switched: the P4 side must follow or nothing is understood any more.
        ESP_RETURN_ON_FALSE(loader_port_change_transmission_rate(baud) == ESP_LOADER_SUCCESS, ESP_FAIL, TAG,
                            "switching the UART to %u baud failed", (unsigned)baud);
        ESP_LOGI(TAG, "ROM loader link at %u baud", (unsigned)baud);
    } else {
        ESP_LOGW(TAG, "the C6 stays at %u baud (slow)", (unsigned)CONNECT_BAUD);
    }
    return ESP_OK;
}

static esp_err_t write_data(const rom_region_t *region)
{
    esp_loader_error_t err = esp_loader_flash_start(region->offset, region->size, BLOCK_SIZE);

    for (uint32_t done = 0; err == ESP_LOADER_SUCCESS && done < region->size; done += BLOCK_SIZE) {
        const uint32_t n = region->size - done < BLOCK_SIZE ? region->size - done : BLOCK_SIZE;

        memcpy(s_fl.block, region->data + done, n); // the flasher wants a writable buffer
        err = esp_loader_flash_write(s_fl.block, n);
    }
    return err == ESP_LOADER_SUCCESS ? ESP_OK : ESP_FAIL;
}

static esp_err_t op_write_region(const rom_region_t *region)
{
    ESP_LOGI(TAG, "%s %s at 0x%x (%u bytes)", region->kind == ROM_REGION_WRITE ? "writing" : "erasing", region->name,
             (unsigned)region->offset, (unsigned)region->size);
    if (region->kind == ROM_REGION_ERASE) {
        return esp_loader_flash_erase_region(region->offset, region->size) == ESP_LOADER_SUCCESS ? ESP_OK : ESP_FAIL;
    }
    return write_data(region);
}

// MD5 of `size` erased bytes.
static void erased_md5(uint32_t size, uint8_t digest[MD5_LEN])
{
    md5_context_t md5;

    memset(s_fl.block, ERASED_BYTE, BLOCK_SIZE);
    esp_rom_md5_init(&md5);
    for (uint32_t done = 0; done < size; done += BLOCK_SIZE) {
        esp_rom_md5_update(&md5, s_fl.block, size - done < BLOCK_SIZE ? size - done : BLOCK_SIZE);
    }
    esp_rom_md5_final(digest, &md5);
}

static esp_err_t op_verify_region(const rom_region_t *region)
{
    esp_loader_error_t err;

    if (region->kind == ROM_REGION_ERASE) {
        uint8_t digest[MD5_LEN];
        char hex[2 * MD5_LEN + 1];

        erased_md5(region->size, digest);
        rom_plan_md5_hex(digest, hex); // the loader compares the hex text, not the raw digest
        err = esp_loader_flash_verify_known_md5(region->offset, region->size, (const uint8_t *)hex);
    } else {
        err = esp_loader_flash_verify(); // MD5 of what flash_start/flash_write just sent
    }
    if (err != ESP_LOADER_SUCCESS) {
        ESP_LOGW(TAG, "verifying %s failed (%d)", region->name, err);
    }
    return err == ESP_LOADER_SUCCESS ? ESP_OK : ESP_ERR_INVALID_CRC;
}

// Runs on every path of a recovery. JUMPER_BOOT goes high-impedance before the C6 is reset, so it boots normally.
static void op_release(void)
{
    release_pin(P4_PIN_JUMPER_BOOT);
    if (s_fl.port_up) {
        esp_loader_reset_target();
        loader_port_esp32_deinit();
        s_fl.port_up = false;
    }
    release_pin(P4_PIN_JUMPER_TX);
    free(s_fl.block);
    s_fl.block = NULL;
}

static const rom_ops_t s_ops = {
    .connect = op_connect,
    .write_region = op_write_region,
    .verify_region = op_verify_region,
    .release = op_release,
};

// Pins and the UART belong to the flasher for the duration; the log receiver gets them back afterwards.
static esp_err_t run_plan(const rom_plan_t *plan)
{
    bool expected = false;

    ESP_RETURN_ON_FALSE(atomic_compare_exchange_strong(&s_busy, &expected, true), ESP_ERR_INVALID_STATE, TAG,
                        "a ROM recovery already runs");
    esp_err_t err = jumper_log_pause();

    if (err == ESP_OK) {
        err = rom_plan_run(plan, &s_ops);
    } else {
        ESP_LOGE(TAG, "the C6 log receiver did not release the UART");
    }
    jumper_log_resume();
    atomic_store(&s_busy, false);
    return err;
}

// No pin is touched, nothing is paused, unless the jumper was seen.
static esp_err_t require_jumper(void)
{
    ESP_RETURN_ON_FALSE(rcp_sync_jumper_present(), ESP_ERR_NOT_SUPPORTED, TAG, "no maintenance jumper");
    return ESP_OK;
}

esp_err_t rcp_sync_rom_recover(void)
{
    rom_plan_t plan;

    ESP_RETURN_ON_ERROR(require_jumper(), TAG, "ROM recovery refused");
    const rcp_sync_base_image_t *image = rcp_sync_base_image();

    ESP_RETURN_ON_FALSE(image, ESP_ERR_INVALID_STATE, TAG, "no base image embedded");
    // Every size is checked before the first byte is written.
    ESP_RETURN_ON_ERROR(rom_plan_build_base(&plan, image->bootloader_start, (size_t)(image->bootloader_end - image->bootloader_start),
                                            image->ptable_start, (size_t)(image->ptable_end - image->ptable_start),
                                            image->updater_start, (size_t)(image->updater_end - image->updater_start)),
                        TAG, "the embedded base image does not fit the C6 layout");
    ESP_LOGW(TAG, "ROM recovery: rewriting the C6 base image");
    const esp_err_t err = run_plan(&plan);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "ROM recovery done");
    } else {
        ESP_LOGE(TAG, "ROM recovery failed: %s", esp_err_to_name(err));
    }
    return err;
}

#ifdef OTBR_TEST_BUILD
esp_err_t rcp_sync_test_c6_erase(uint32_t offset, uint32_t size)
{
    rom_plan_t plan;

    ESP_RETURN_ON_ERROR(require_jumper(), TAG, "erase refused");
    ESP_RETURN_ON_ERROR(rom_plan_build_erase(&plan, "range", offset, size), TAG, "bad erase range");
    return run_plan(&plan);
}
#endif
