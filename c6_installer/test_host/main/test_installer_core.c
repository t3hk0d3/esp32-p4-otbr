#include <stdlib.h>
#include <string.h>
#include "unity.h"
#include "unity_test_runner.h"
#include "c6_board.h"
#include "installer_core.h"

/* ---- fake 4 MB flash with an operation log ---- */

#define FLASH_SIZE 0x400000
#define LOG_MAX    4096

typedef struct {
    char op; /* 'e' erase, 'w' write, 'v' verify_image */
    uint32_t addr;
    uint32_t len;
} op_t;

static uint8_t *s_flash;
static op_t s_log[LOG_MAX];
static int s_log_n;
static uint32_t s_corrupt_write_at; /* the next writes covering this address flip a byte there */
static int s_corrupt_writes_left;
static esp_err_t s_verify_image_result;

static void log_op(char op, uint32_t addr, uint32_t len)
{
    if (s_log_n < LOG_MAX) {
        s_log[s_log_n++] = (op_t) {
            op, addr, len
        };
    }
}

static esp_err_t f_read(uint32_t addr, void *buf, size_t len)
{
    TEST_ASSERT_TRUE(addr + len <= FLASH_SIZE);
    memcpy(buf, s_flash + addr, len);
    return ESP_OK;
}

static esp_err_t f_erase(uint32_t addr, size_t len)
{
    TEST_ASSERT_EQUAL_UINT32(0, addr % INSTALLER_SECTOR);
    TEST_ASSERT_EQUAL_UINT32(0, len % INSTALLER_SECTOR);
    memset(s_flash + addr, 0xFF, len);
    log_op('e', addr, (uint32_t)len);
    return ESP_OK;
}

static esp_err_t f_write(uint32_t addr, const void *buf, size_t len)
{
    memcpy(s_flash + addr, buf, len);
    if (s_corrupt_writes_left > 0 && s_corrupt_write_at >= addr && s_corrupt_write_at < addr + len) {
        s_flash[s_corrupt_write_at] ^= 0x5A;
        s_corrupt_writes_left--;
    }
    log_op('w', addr, (uint32_t)len);
    return ESP_OK;
}

static esp_err_t f_verify_image(uint32_t addr, uint32_t room)
{
    log_op('v', addr, room);
    return s_verify_image_result;
}

static const installer_flash_t s_ops = {
    .read = f_read, .erase = f_erase, .write = f_write, .verify_image = f_verify_image,
};

/* ---- the embedded images ---- */

static uint8_t s_bootloader[22464];
static uint8_t s_ptable[3072];
static uint8_t s_updater[228336];
static installer_images_t s_images;

static void fill(uint8_t *p, size_t len, uint8_t seed)
{
    for (size_t i = 0; i < len; i++) {
        p[i] = (uint8_t)(seed + i * 7u);
    }
}

static void reset_flash(void)
{
    if (!s_flash) {
        s_flash = malloc(FLASH_SIZE);
        TEST_ASSERT_NOT_NULL(s_flash);
        fill(s_bootloader, sizeof(s_bootloader), 0x40);
        fill(s_ptable, sizeof(s_ptable), 0x41);
        fill(s_updater, sizeof(s_updater), 0x42);
    }
    memset(s_flash, 0xA5, FLASH_SIZE); /* the old ESP-Hosted contents everywhere */
    s_images = (installer_images_t) {
        .bootloader = {s_bootloader, sizeof(s_bootloader)},
        .ptable = {s_ptable, sizeof(s_ptable)},
        .updater = {s_updater, sizeof(s_updater)},
    };
    s_log_n = 0;
    s_corrupt_writes_left = 0;
    s_verify_image_result = ESP_OK;
}

static bool flash_holds(uint32_t addr, const uint8_t *data, size_t len)
{
    return memcmp(s_flash + addr, data, len) == 0;
}

/* Index of the first erase of `addr` in the log, -1 if none. */
static int first_erase_of(uint32_t addr)
{
    for (int i = 0; i < s_log_n; i++) {
        if (s_log[i].op == 'e' && s_log[i].addr == addr) {
            return i;
        }
    }
    return -1;
}

static int count_ops(char op)
{
    int n = 0;
    for (int i = 0; i < s_log_n; i++) {
        n += s_log[i].op == op;
    }
    return n;
}

static bool all_ff(uint32_t addr, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++) {
        if (s_flash[addr + i] != 0xFF) {
            return false;
        }
    }
    return true;
}

static const installer_slot_t k_stock_slots[] = {
    {INSTALLER_STOCK_OTA0_OFFSET, INSTALLER_STOCK_SLOT_SIZE},
    {INSTALLER_STOCK_OTA1_OFFSET, INSTALLER_STOCK_SLOT_SIZE},
};

#define IMAGE_LEN 0x65123u

/* ---- installer_decide ---- */

TEST_CASE("On running in stock ota_1 with a stock table: INSTALL", "[installer]")
{
    const installer_situation_t s = {
        .running = k_stock_slots[1], .image_len = IMAGE_LEN, .slots = k_stock_slots, .slot_count = 2,
    };
    installer_slot_t to = {0};

    TEST_ASSERT_EQUAL(INSTALLER_INSTALL, installer_decide(&s, &to));
}

TEST_CASE("On running in stock ota_0: RELOCATE to stock ota_1", "[installer]")
{
    const installer_situation_t s = {
        .running = k_stock_slots[0], .image_len = IMAGE_LEN, .slots = k_stock_slots, .slot_count = 2,
    };
    installer_slot_t to = {0};

    TEST_ASSERT_EQUAL(INSTALLER_RELOCATE, installer_decide(&s, &to));
    TEST_ASSERT_EQUAL_HEX32(INSTALLER_STOCK_OTA1_OFFSET, to.offset);
    TEST_ASSERT_EQUAL_HEX32(INSTALLER_STOCK_SLOT_SIZE, to.size);
}

TEST_CASE("On our partition table already at 0x8000: ALREADY_OURS wherever it runs", "[installer]")
{
    installer_situation_t s = {
        .running = k_stock_slots[1], .image_len = IMAGE_LEN, .slots = k_stock_slots, .slot_count = 2, .ptable_is_ours = true,
    };
    installer_slot_t to = {0};

    TEST_ASSERT_EQUAL(INSTALLER_ALREADY_OURS, installer_decide(&s, &to));
    s.running = k_stock_slots[0];
    TEST_ASSERT_EQUAL(INSTALLER_ALREADY_OURS, installer_decide(&s, &to));
}

TEST_CASE("On running in stock ota_0 with no clear slot to relocate into: STUCK", "[installer]")
{
    const installer_slot_t small[] = {k_stock_slots[0], {INSTALLER_STOCK_OTA1_OFFSET, 0x10000}};
    const installer_situation_t s = {.running = k_stock_slots[0], .image_len = IMAGE_LEN, .slots = small, .slot_count = 2};
    installer_slot_t to = {0};

    TEST_ASSERT_EQUAL(INSTALLER_STUCK, installer_decide(&s, &to));
}

TEST_CASE("On a running slot starting inside the first ota_0 sector: never INSTALL", "[installer]")
{
    const installer_slot_t odd[] = {{C6_FLASH_OTA0_OFFSET, 0x100000}};
    const installer_situation_t s = {.running = odd[0], .image_len = 0x20000, .slots = odd, .slot_count = 1};
    installer_slot_t to = {0};

    TEST_ASSERT_EQUAL(INSTALLER_STUCK, installer_decide(&s, &to));
}

/* ---- installer_images_check ---- */

TEST_CASE("On images within their rooms: ESP_OK", "[installer]")
{
    reset_flash();
    TEST_ASSERT_EQUAL(ESP_OK, installer_images_check(&s_images));
}

TEST_CASE("On an image larger than its destination or missing: ESP_ERR_INVALID_SIZE", "[installer]")
{
    reset_flash();
    s_images.ptable.len = INSTALLER_PTABLE_ROOM + 1;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, installer_images_check(&s_images));
    reset_flash();
    s_images.bootloader.len = INSTALLER_BOOTLOADER_ROOM + 1;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, installer_images_check(&s_images));
    reset_flash();
    s_images.updater.len = C6_FLASH_TEST_SIZE + 1;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, installer_images_check(&s_images));
    reset_flash();
    s_images.updater.data = NULL;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, installer_images_check(&s_images));
}

/* ---- installer_install ---- */

TEST_CASE("On a stock C6: writes updater, bootloader, partition table, then erases ota_0's first sector and otadata, in that order", "[installer]")
{
    reset_flash();
    TEST_ASSERT_EQUAL(ESP_OK, installer_install(&s_ops, &s_images));

    const int upd = first_erase_of(C6_FLASH_TEST_OFFSET);
    const int bl = first_erase_of(C6_FLASH_BOOTLOADER_OFFSET);
    const int pt = first_erase_of(C6_FLASH_PTABLE_OFFSET);
    const int ota0 = first_erase_of(C6_FLASH_OTA0_OFFSET);
    const int od = first_erase_of(C6_FLASH_OTADATA_OFFSET);
    TEST_ASSERT_TRUE(upd >= 0 && upd < bl && bl < pt && pt < ota0 && ota0 < od);
    TEST_ASSERT_TRUE(flash_holds(C6_FLASH_TEST_OFFSET, s_updater, sizeof(s_updater)));
    TEST_ASSERT_TRUE(flash_holds(C6_FLASH_BOOTLOADER_OFFSET, s_bootloader, sizeof(s_bootloader)));
    TEST_ASSERT_TRUE(flash_holds(C6_FLASH_PTABLE_OFFSET, s_ptable, sizeof(s_ptable)));
    TEST_ASSERT_TRUE(all_ff(C6_FLASH_OTA0_OFFSET, INSTALLER_SECTOR));
    TEST_ASSERT_TRUE(all_ff(C6_FLASH_OTADATA_OFFSET, C6_FLASH_OTADATA_SIZE));
}

TEST_CASE("On a stock C6: the updater gets an image verification at test", "[installer]")
{
    reset_flash();
    TEST_ASSERT_EQUAL(ESP_OK, installer_install(&s_ops, &s_images));
    TEST_ASSERT_EQUAL(1, count_ops('v'));
    for (int i = 0; i < s_log_n; i++) {
        if (s_log[i].op == 'v') {
            TEST_ASSERT_EQUAL_HEX32(C6_FLASH_TEST_OFFSET, s_log[i].addr);
            TEST_ASSERT_EQUAL_HEX32(C6_FLASH_TEST_SIZE, s_log[i].len);
        }
    }
}

TEST_CASE("On a stock C6: nothing at or after INSTALLER_TARGET_END is written or erased", "[installer]")
{
    reset_flash();
    TEST_ASSERT_EQUAL(ESP_OK, installer_install(&s_ops, &s_images));
    for (int i = 0; i < s_log_n; i++) {
        if (s_log[i].op != 'v') {
            TEST_ASSERT_TRUE(s_log[i].addr + s_log[i].len <= INSTALLER_TARGET_END);
        }
    }
}

TEST_CASE("On regions already holding the right content (repeat after a power cut): skips them", "[installer]")
{
    reset_flash();
    TEST_ASSERT_EQUAL(ESP_OK, installer_install(&s_ops, &s_images));
    s_log_n = 0;
    TEST_ASSERT_EQUAL(ESP_OK, installer_install(&s_ops, &s_images));
    TEST_ASSERT_EQUAL(0, count_ops('w'));
    TEST_ASSERT_EQUAL(-1, first_erase_of(C6_FLASH_TEST_OFFSET));
    TEST_ASSERT_EQUAL(-1, first_erase_of(C6_FLASH_BOOTLOADER_OFFSET));
    TEST_ASSERT_EQUAL(-1, first_erase_of(C6_FLASH_PTABLE_OFFSET));
}

TEST_CASE("On a power cut after the updater: the next run writes only what is still missing", "[installer]")
{
    reset_flash();
    memcpy(s_flash + C6_FLASH_TEST_OFFSET, s_updater, sizeof(s_updater));
    TEST_ASSERT_EQUAL(ESP_OK, installer_install(&s_ops, &s_images));
    TEST_ASSERT_EQUAL(-1, first_erase_of(C6_FLASH_TEST_OFFSET));
    TEST_ASSERT_TRUE(first_erase_of(C6_FLASH_BOOTLOADER_OFFSET) >= 0);
}

TEST_CASE("On a region failing its compare once: the second try succeeds", "[installer]")
{
    reset_flash();
    s_corrupt_write_at = C6_FLASH_TEST_OFFSET + 5;
    s_corrupt_writes_left = 1;
    TEST_ASSERT_EQUAL(ESP_OK, installer_install(&s_ops, &s_images));
    TEST_ASSERT_TRUE(flash_holds(C6_FLASH_TEST_OFFSET, s_updater, sizeof(s_updater)));
}

TEST_CASE("On a region failing its compare twice: stops with an error, later regions untouched", "[installer]")
{
    reset_flash();
    s_corrupt_write_at = C6_FLASH_TEST_OFFSET + 5;
    s_corrupt_writes_left = 2;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_CRC, installer_install(&s_ops, &s_images));
    TEST_ASSERT_EQUAL(-1, first_erase_of(C6_FLASH_BOOTLOADER_OFFSET));
    TEST_ASSERT_EQUAL(-1, first_erase_of(C6_FLASH_PTABLE_OFFSET));
    TEST_ASSERT_EQUAL(-1, first_erase_of(C6_FLASH_OTADATA_OFFSET));
}

TEST_CASE("On the updater failing its image verification twice: stops before the bootloader", "[installer]")
{
    reset_flash();
    s_verify_image_result = ESP_ERR_INVALID_STATE;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, installer_install(&s_ops, &s_images));
    TEST_ASSERT_EQUAL(2, count_ops('v'));
    TEST_ASSERT_EQUAL(-1, first_erase_of(C6_FLASH_BOOTLOADER_OFFSET));
}

/* ---- installer_ptable_is_ours ---- */

TEST_CASE("On the embedded partition table at 0x8000: ptable_is_ours is true, otherwise false", "[installer]")
{
    reset_flash();
    TEST_ASSERT_FALSE(installer_ptable_is_ours(&s_ops, &s_images));
    memcpy(s_flash + C6_FLASH_PTABLE_OFFSET, s_ptable, sizeof(s_ptable));
    TEST_ASSERT_TRUE(installer_ptable_is_ours(&s_ops, &s_images));
}

/* ---- installer_relocate ---- */

TEST_CASE("On running in stock ota_0: relocate copies the image into stock ota_1, writing nothing else", "[installer]")
{
    reset_flash();
    fill(s_flash + INSTALLER_STOCK_OTA0_OFFSET, IMAGE_LEN, 0x33);

    TEST_ASSERT_EQUAL(ESP_OK, installer_relocate(&s_ops, INSTALLER_STOCK_OTA0_OFFSET, k_stock_slots[1], IMAGE_LEN));
    TEST_ASSERT_EQUAL_MEMORY(s_flash + INSTALLER_STOCK_OTA0_OFFSET, s_flash + INSTALLER_STOCK_OTA1_OFFSET, IMAGE_LEN);
    for (int i = 0; i < s_log_n; i++) {
        TEST_ASSERT_TRUE(s_log[i].addr >= INSTALLER_STOCK_OTA1_OFFSET);
    }
}

TEST_CASE("On relocating more than the target slot holds: ESP_ERR_INVALID_SIZE, nothing written", "[installer]")
{
    reset_flash();
    const installer_slot_t small = {INSTALLER_STOCK_OTA1_OFFSET, 0x1000};

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, installer_relocate(&s_ops, INSTALLER_STOCK_OTA0_OFFSET, small, 0x2000));
    TEST_ASSERT_EQUAL(0, s_log_n);
}

void app_main(void)
{
    UNITY_BEGIN();
    unity_run_all_tests();
    int failures = (int)UNITY_END();
    free(s_flash);
    exit(failures == 0 ? 0 : 1);
}
