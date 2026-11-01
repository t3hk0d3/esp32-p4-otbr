#include "rom_plan.h"

#include <stdbool.h>

#include "c6_board.h"

#define FLASH_SECTOR 0x1000u

// Room each blob has: up to the next thing in the C6 layout (partitions_c6.csv).
#define BOOTLOADER_ROOM (C6_FLASH_PTABLE_OFFSET - C6_FLASH_BOOTLOADER_OFFSET)
#define PTABLE_ROOM     (C6_FLASH_NVS_OFFSET - C6_FLASH_PTABLE_OFFSET)

static bool fits(size_t len, size_t room)
{
    return len > 0 && len <= room;
}

esp_err_t rom_plan_build_base(rom_plan_t *plan, const uint8_t *bootloader, size_t bootloader_len, const uint8_t *ptable,
                              size_t ptable_len, const uint8_t *updater, size_t updater_len)
{
    if (!plan) {
        return ESP_ERR_INVALID_ARG;
    }
    plan->count = 0;
    if (!bootloader || !ptable || !updater) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!fits(bootloader_len, BOOTLOADER_ROOM) || !fits(ptable_len, PTABLE_ROOM) || !fits(updater_len, C6_FLASH_TEST_SIZE)) {
        return ESP_ERR_INVALID_SIZE;
    }
    plan->regions[0] = (rom_region_t) {
        "bootloader", ROM_REGION_WRITE, C6_FLASH_BOOTLOADER_OFFSET, (uint32_t)bootloader_len, bootloader
    };
    plan->regions[1] = (rom_region_t) {
        "partition table", ROM_REGION_WRITE, C6_FLASH_PTABLE_OFFSET, (uint32_t)ptable_len, ptable
    };
    plan->regions[2] = (rom_region_t) {
        "otadata", ROM_REGION_ERASE, C6_FLASH_OTADATA_OFFSET, C6_FLASH_OTADATA_SIZE, NULL
    };
    plan->regions[3] = (rom_region_t) {
        "updater", ROM_REGION_WRITE, C6_FLASH_TEST_OFFSET, (uint32_t)updater_len, updater
    };
    plan->count = 4;
    return ESP_OK;
}

esp_err_t rom_plan_build_erase(rom_plan_t *plan, const char *name, uint32_t offset, uint32_t size)
{
    if (!plan) {
        return ESP_ERR_INVALID_ARG;
    }
    plan->count = 0;
    if (!name || size == 0 || offset % FLASH_SECTOR != 0 || size % FLASH_SECTOR != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    plan->regions[0] = (rom_region_t) {
        name, ROM_REGION_ERASE, offset, size, NULL
    };
    plan->count = 1;
    return ESP_OK;
}

void rom_plan_md5_hex(const uint8_t digest[16], char out[33])
{
    static const char HEX[] = "0123456789abcdef";

    for (int i = 0; i < 16; i++) {
        out[2 * i] = HEX[digest[i] >> 4];
        out[2 * i + 1] = HEX[digest[i] & 0xf];
    }
    out[32] = '\0';
}

uint32_t rom_plan_baud_for_rung(unsigned rung)
{
    static const uint32_t LADDER[ROM_BAUD_LADDER_LEN] = {1500000u, 460800u, 115200u};

    return LADDER[rung < ROM_BAUD_LADDER_LEN ? rung : ROM_BAUD_LADDER_LEN - 1];
}

// One region with up to ROM_REGION_ATTEMPTS tries; every retry reconnects one rung further down the ladder. The count holds
// only while the reconnects succeed: a failing reconnect ends the run at once.
static esp_err_t do_region(const rom_ops_t *ops, const rom_region_t *region, unsigned *rung)
{
    esp_err_t err = ESP_FAIL;

    for (int attempt = 0; attempt < ROM_REGION_ATTEMPTS; attempt++) {
        err = ops->write_region(region);
        if (err == ESP_OK) {
            err = ops->verify_region(region);
        }
        if (err == ESP_OK) {
            return ESP_OK;
        }
        if (attempt + 1 < ROM_REGION_ATTEMPTS) {
            (*rung)++;
            const esp_err_t reconnect_err = ops->connect(rom_plan_baud_for_rung(*rung));

            if (reconnect_err != ESP_OK) {
                return reconnect_err;
            }
        }
    }
    return err;
}

esp_err_t rom_plan_run(const rom_plan_t *plan, const rom_ops_t *ops)
{
    if (!plan || plan->count == 0 || plan->count > ROM_PLAN_MAX_REGIONS || !ops || !ops->connect || !ops->write_region ||
            !ops->verify_region || !ops->release) {
        return ESP_ERR_INVALID_ARG;
    }
    unsigned rung = 0;
    esp_err_t err = ops->connect(rom_plan_baud_for_rung(rung));

    for (size_t i = 0; err == ESP_OK && i < plan->count; i++) {
        err = do_region(ops, &plan->regions[i], &rung);
    }
    ops->release();
    return err;
}
