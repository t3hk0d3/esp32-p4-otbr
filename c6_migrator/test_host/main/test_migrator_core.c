#include <stdlib.h>
#include <string.h>
#include "unity.h"
#include "unity_test_runner.h"
#include "c6_board.h"
#include "c6_bundle.h"
#include "migrator_core.h"

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
static uint32_t s_corrupt_write_at; /* the next write covering this address flips a byte there (0 = off) */
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
    TEST_ASSERT_EQUAL_UINT32(0, addr % MIGRATOR_SECTOR);
    TEST_ASSERT_EQUAL_UINT32(0, len % MIGRATOR_SECTOR);
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

static const migrator_flash_t s_ops = {
    .read = f_read, .erase = f_erase, .write = f_write, .verify_image = f_verify_image,
};

/* ---- a transfer in stock ota_1: migrator image, padding, header, blobs ---- */

#define IMAGE_LEN 0x25123u
static const uint32_t k_lengths[C6_BUNDLE_ENTRIES] = {22464, 3072, 228336, 277824};
static c6_bundle_t s_bundle;
static uint32_t s_data_addr;

static void fill_blob(uint8_t *p, uint32_t len, uint8_t seed)
{
    for (uint32_t i = 0; i < len; i++) {
        p[i] = (uint8_t)(seed + i * 7u);
    }
}

/* Lay out the transfer at `slot` and return the address of its data. */
static uint32_t place_transfer(uint32_t slot)
{
    const uint32_t header_at = slot + c6_bundle_header_offset(IMAGE_LEN);
    uint32_t addr = header_at + C6_BUNDLE_HEADER_LEN;

    memset(&s_bundle, 0, sizeof(s_bundle));
    memset(s_flash + slot, 0x33, IMAGE_LEN); /* the migrator image itself */
    for (int i = 0; i < C6_BUNDLE_ENTRIES; i++) {
        s_bundle.entries[i].kind = (uint32_t)(i + 1);
        s_bundle.entries[i].length = k_lengths[i];
        fill_blob(s_flash + addr, k_lengths[i], (uint8_t)(0x40 + i));
        TEST_ASSERT_EQUAL(ESP_OK, c6_bundle_sha256(s_flash + addr, k_lengths[i], s_bundle.entries[i].sha256));
        addr += k_lengths[i];
    }
    TEST_ASSERT_EQUAL(ESP_OK, c6_bundle_header_encode(&s_bundle, s_flash + header_at, C6_BUNDLE_HEADER_LEN));
    return header_at + C6_BUNDLE_HEADER_LEN;
}

static uint32_t transfer_used(void)
{
    return c6_bundle_header_offset(IMAGE_LEN) + C6_BUNDLE_HEADER_LEN + c6_bundle_data_len(&s_bundle);
}

static void reset_flash(void)
{
    if (!s_flash) {
        s_flash = malloc(FLASH_SIZE);
        TEST_ASSERT_NOT_NULL(s_flash);
    }
    memset(s_flash, 0xA5, FLASH_SIZE); /* the old ESP-Hosted contents everywhere */
    s_log_n = 0;
    s_corrupt_writes_left = 0;
    s_verify_image_result = ESP_OK;
    s_data_addr = place_transfer(C6_BUNDLE_STOCK_OTA1_OFFSET);
}

static bool target_holds(c6_bundle_kind_t kind)
{
    const uint32_t src = migrator_blob_addr(&s_bundle, s_data_addr, kind);
    return memcmp(s_flash + c6_bundle_target_offset(kind), s_flash + src, s_bundle.entries[kind - 1].length) == 0;
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

static const migrator_slot_t k_stock_slots[] = {
    {C6_BUNDLE_STOCK_OTA0_OFFSET, C6_BUNDLE_STOCK_SLOT_SIZE},
    {C6_BUNDLE_STOCK_OTA1_OFFSET, C6_BUNDLE_STOCK_SLOT_SIZE},
};

/* ---- migrator_decide ---- */

TEST_CASE("On running in stock ota_1 with a stock table: INSTALL", "[migrator]")
{
    const migrator_situation_t s = {
        .running = k_stock_slots[1], .used = 0xB0000, .slots = k_stock_slots, .slot_count = 2,
    };
    migrator_slot_t to = {0};

    TEST_ASSERT_EQUAL(MIGRATOR_INSTALL, migrator_decide(&s, &to));
}

TEST_CASE("On running in stock ota_0: RELOCATE to stock ota_1", "[migrator]")
{
    const migrator_situation_t s = {
        .running = k_stock_slots[0], .used = 0xB0000, .slots = k_stock_slots, .slot_count = 2,
    };
    migrator_slot_t to = {0};

    TEST_ASSERT_EQUAL(MIGRATOR_RELOCATE, migrator_decide(&s, &to));
    TEST_ASSERT_EQUAL_HEX32(C6_BUNDLE_STOCK_OTA1_OFFSET, to.offset);
    TEST_ASSERT_EQUAL_HEX32(C6_BUNDLE_STOCK_SLOT_SIZE, to.size);
}

TEST_CASE("On our partition table already at 0x8000: ALREADY_OURS wherever it runs", "[migrator]")
{
    migrator_situation_t s = {
        .running = k_stock_slots[1], .used = 0xB0000, .slots = k_stock_slots, .slot_count = 2, .ptable_is_ours = true,
    };
    migrator_slot_t to = {0};

    TEST_ASSERT_EQUAL(MIGRATOR_ALREADY_OURS, migrator_decide(&s, &to));
    s.running = k_stock_slots[0];
    TEST_ASSERT_EQUAL(MIGRATOR_ALREADY_OURS, migrator_decide(&s, &to));
}

TEST_CASE("On running in stock ota_0 with no clear slot to relocate into: STUCK", "[migrator]")
{
    const migrator_slot_t small[] = {k_stock_slots[0], {C6_BUNDLE_STOCK_OTA1_OFFSET, 0x10000}};
    const migrator_situation_t s = {.running = k_stock_slots[0], .used = 0xB0000, .slots = small, .slot_count = 2};
    migrator_slot_t to = {0};

    TEST_ASSERT_EQUAL(MIGRATOR_STUCK, migrator_decide(&s, &to));
}

TEST_CASE("On a running slot that starts clear but whose transfer reaches into a target: never INSTALL", "[migrator]")
{
    /* A slot starting below the target end with the transfer crossing it is not clear. */
    const migrator_slot_t odd[] = {{0x180000, 0x100000}};
    const migrator_situation_t s = {.running = odd[0], .used = 0x20000, .slots = odd, .slot_count = 1};
    migrator_slot_t to = {0};

    TEST_ASSERT_EQUAL(MIGRATOR_STUCK, migrator_decide(&s, &to));
}

/* ---- migrator_install ---- */

TEST_CASE("On a valid bundle on a stock C6: writes RCP, updater, bootloader, partition table, then erases otadata, in that order", "[migrator]")
{
    reset_flash();
    TEST_ASSERT_EQUAL(ESP_OK, migrator_check_blobs(&s_ops, &s_bundle, s_data_addr));
    TEST_ASSERT_EQUAL(ESP_OK, migrator_install(&s_ops, &s_bundle, s_data_addr));

    const int rcp = first_erase_of(C6_FLASH_OTA0_OFFSET);
    const int upd = first_erase_of(C6_FLASH_TEST_OFFSET);
    const int bl = first_erase_of(C6_FLASH_BOOTLOADER_OFFSET);
    const int pt = first_erase_of(C6_FLASH_PTABLE_OFFSET);
    const int od = first_erase_of(C6_FLASH_OTADATA_OFFSET);
    TEST_ASSERT_TRUE(rcp >= 0 && rcp < upd && upd < bl && bl < pt && pt < od);
    TEST_ASSERT_EQUAL_UINT32(C6_FLASH_OTADATA_SIZE, s_log[od].len);
    for (int k = C6_BUNDLE_BOOTLOADER; k <= C6_BUNDLE_RCP; k++) {
        TEST_ASSERT_TRUE_MESSAGE(target_holds((c6_bundle_kind_t)k), "blob not in place");
    }
    for (uint32_t i = 0; i < C6_FLASH_OTADATA_SIZE; i++) {
        TEST_ASSERT_EQUAL_HEX8(0xFF, s_flash[C6_FLASH_OTADATA_OFFSET + i]);
    }
}

TEST_CASE("On a valid bundle: the RCP and the updater get an image verification at their targets", "[migrator]")
{
    reset_flash();
    TEST_ASSERT_EQUAL(ESP_OK, migrator_install(&s_ops, &s_bundle, s_data_addr));
    bool rcp = false, upd = false;
    for (int i = 0; i < s_log_n; i++) {
        rcp |= s_log[i].op == 'v' && s_log[i].addr == C6_FLASH_OTA0_OFFSET;
        upd |= s_log[i].op == 'v' && s_log[i].addr == C6_FLASH_TEST_OFFSET;
    }
    TEST_ASSERT_TRUE(rcp);
    TEST_ASSERT_TRUE(upd);
}

TEST_CASE("On a valid bundle: nothing at or after stock ota_1 is written or erased", "[migrator]")
{
    reset_flash();
    TEST_ASSERT_EQUAL(ESP_OK, migrator_install(&s_ops, &s_bundle, s_data_addr));
    for (int i = 0; i < s_log_n; i++) {
        if (s_log[i].op != 'v') {
            TEST_ASSERT_TRUE(s_log[i].addr + s_log[i].len <= C6_BUNDLE_TARGET_END);
        }
    }
}

TEST_CASE("On a blob whose SHA-256 does not match: check_blobs fails with ESP_ERR_INVALID_CRC and writes nothing", "[migrator]")
{
    reset_flash();
    s_flash[migrator_blob_addr(&s_bundle, s_data_addr, C6_BUNDLE_UPDATER) + 1000] ^= 0x01;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_CRC, migrator_check_blobs(&s_ops, &s_bundle, s_data_addr));
    TEST_ASSERT_EQUAL(0, count_ops('e') + count_ops('w'));
}

TEST_CASE("On regions already holding the right content (repeat after a power cut): skips them", "[migrator]")
{
    reset_flash();
    TEST_ASSERT_EQUAL(ESP_OK, migrator_install(&s_ops, &s_bundle, s_data_addr));
    s_log_n = 0;
    TEST_ASSERT_EQUAL(ESP_OK, migrator_install(&s_ops, &s_bundle, s_data_addr));
    TEST_ASSERT_EQUAL(0, count_ops('w'));
    TEST_ASSERT_EQUAL(-1, first_erase_of(C6_FLASH_BOOTLOADER_OFFSET));
    TEST_ASSERT_EQUAL(-1, first_erase_of(C6_FLASH_PTABLE_OFFSET));
}

TEST_CASE("On a power cut after the RCP: the next run writes only what is still missing", "[migrator]")
{
    reset_flash();
    const uint32_t rcp_src = migrator_blob_addr(&s_bundle, s_data_addr, C6_BUNDLE_RCP);
    memcpy(s_flash + C6_FLASH_OTA0_OFFSET, s_flash + rcp_src, k_lengths[3]);
    TEST_ASSERT_EQUAL(ESP_OK, migrator_install(&s_ops, &s_bundle, s_data_addr));
    TEST_ASSERT_EQUAL(-1, first_erase_of(C6_FLASH_OTA0_OFFSET));
    TEST_ASSERT_TRUE(first_erase_of(C6_FLASH_TEST_OFFSET) >= 0);
}

TEST_CASE("On a region failing its compare once: the second try succeeds", "[migrator]")
{
    reset_flash();
    s_corrupt_write_at = C6_FLASH_TEST_OFFSET + 5;
    s_corrupt_writes_left = 1;
    TEST_ASSERT_EQUAL(ESP_OK, migrator_install(&s_ops, &s_bundle, s_data_addr));
    TEST_ASSERT_TRUE(target_holds(C6_BUNDLE_UPDATER));
}

TEST_CASE("On a region failing its compare twice: stops with an error, later regions untouched", "[migrator]")
{
    reset_flash();
    s_corrupt_write_at = C6_FLASH_TEST_OFFSET + 5;
    s_corrupt_writes_left = 2;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_CRC, migrator_install(&s_ops, &s_bundle, s_data_addr));
    TEST_ASSERT_EQUAL(-1, first_erase_of(C6_FLASH_BOOTLOADER_OFFSET));
    TEST_ASSERT_EQUAL(-1, first_erase_of(C6_FLASH_PTABLE_OFFSET));
    TEST_ASSERT_EQUAL(-1, first_erase_of(C6_FLASH_OTADATA_OFFSET));
}

TEST_CASE("On the RCP failing its image verification twice: stops before the updater", "[migrator]")
{
    reset_flash();
    s_verify_image_result = ESP_ERR_INVALID_STATE;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, migrator_install(&s_ops, &s_bundle, s_data_addr));
    TEST_ASSERT_EQUAL(-1, first_erase_of(C6_FLASH_TEST_OFFSET));
}

/* ---- migrator_ptable_is_ours ---- */

TEST_CASE("On the bundle's partition table at 0x8000: ptable_is_ours is true, otherwise false", "[migrator]")
{
    reset_flash();
    TEST_ASSERT_FALSE(migrator_ptable_is_ours(&s_ops, &s_bundle, s_data_addr));
    memcpy(s_flash + C6_FLASH_PTABLE_OFFSET, s_flash + migrator_blob_addr(&s_bundle, s_data_addr, C6_BUNDLE_PTABLE),
           k_lengths[1]);
    TEST_ASSERT_TRUE(migrator_ptable_is_ours(&s_ops, &s_bundle, s_data_addr));
}

/* ---- migrator_relocate ---- */

TEST_CASE("On running in stock ota_0: relocate copies image and bundle into stock ota_1", "[migrator]")
{
    reset_flash();
    memset(s_flash + C6_BUNDLE_STOCK_OTA1_OFFSET, 0xA5, C6_BUNDLE_STOCK_SLOT_SIZE);
    s_data_addr = place_transfer(C6_BUNDLE_STOCK_OTA0_OFFSET);
    const uint32_t used = transfer_used();

    TEST_ASSERT_EQUAL(ESP_OK, migrator_relocate(&s_ops, C6_BUNDLE_STOCK_OTA0_OFFSET, k_stock_slots[1], used));
    TEST_ASSERT_EQUAL_MEMORY(s_flash + C6_BUNDLE_STOCK_OTA0_OFFSET, s_flash + C6_BUNDLE_STOCK_OTA1_OFFSET, used);
    for (int i = 0; i < s_log_n; i++) {
        if (s_log[i].op != 'v') {
            TEST_ASSERT_TRUE(s_log[i].addr >= C6_BUNDLE_STOCK_OTA1_OFFSET);
        }
    }
}

TEST_CASE("On relocating more than the target slot holds: ESP_ERR_INVALID_SIZE, nothing written", "[migrator]")
{
    reset_flash();
    const migrator_slot_t small = {C6_BUNDLE_STOCK_OTA1_OFFSET, 0x1000};

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, migrator_relocate(&s_ops, C6_BUNDLE_STOCK_OTA0_OFFSET, small, 0x2000));
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
