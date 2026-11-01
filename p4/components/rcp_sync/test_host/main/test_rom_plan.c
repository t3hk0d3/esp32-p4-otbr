// ROM recovery plan and runner (spec 5.10) against a scripted flasher.
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "unity.h"
#include "c6_board.h"
#include "rom_plan.h"

#define OK 0

static const uint8_t blob[C6_FLASH_TEST_SIZE + 1];

/* ---- md5 text ---- */

TEST_CASE("On a raw digest: renders 32 lowercase hex characters and a terminator", "[rcp_sync][rom_plan]")
{
    const uint8_t digest[16] = {0x84, 0xd0, 0x4c, 0x9d, 0x6c, 0xc8, 0xef, 0x35, 0xbf, 0x82, 0x5d, 0x51, 0xa5, 0x27, 0x76, 0x99};
    char hex[33];

    memset(hex, 'x', sizeof(hex));
    rom_plan_md5_hex(digest, hex);
    TEST_ASSERT_EQUAL_STRING("84d04c9d6cc8ef35bf825d51a5277699", hex);
}

TEST_CASE("On a raw digest with zero nibbles: keeps the leading zeros and the all-zero bytes", "[rcp_sync][rom_plan]")
{
    const uint8_t digest[16] = {0x00, 0x0f, 0x01, 0x10, 0x0a, 0xa0, 0x00, 0x00, 0x09, 0x90, 0x00, 0xf0, 0x07, 0x00, 0x0e, 0x00};
    char hex[33];

    memset(hex, 'x', sizeof(hex));
    rom_plan_md5_hex(digest, hex);
    TEST_ASSERT_EQUAL_STRING("000f01100aa00000099000f007000e00", hex);
}

/* ---- plan ---- */

static rom_plan_t plan;

TEST_CASE("On base image of valid sizes: plans bootloader, partition table, otadata erase and updater, in that order", "[rcp_sync][rom_plan]")
{
    TEST_ASSERT_EQUAL(ESP_OK, rom_plan_build_base(&plan, blob, 0x5000, blob, 0xc00, blob, 0x30000));
    TEST_ASSERT_EQUAL(4, plan.count);
    TEST_ASSERT_EQUAL_STRING("bootloader", plan.regions[0].name);
    TEST_ASSERT_EQUAL(ROM_REGION_WRITE, plan.regions[0].kind);
    TEST_ASSERT_EQUAL_UINT32(C6_FLASH_BOOTLOADER_OFFSET, plan.regions[0].offset);
    TEST_ASSERT_EQUAL_UINT32(0x5000, plan.regions[0].size);
    TEST_ASSERT_EQUAL_STRING("partition table", plan.regions[1].name);
    TEST_ASSERT_EQUAL_UINT32(C6_FLASH_PTABLE_OFFSET, plan.regions[1].offset);
    TEST_ASSERT_EQUAL_STRING("otadata", plan.regions[2].name);
    TEST_ASSERT_EQUAL(ROM_REGION_ERASE, plan.regions[2].kind);
    TEST_ASSERT_EQUAL_UINT32(C6_FLASH_OTADATA_OFFSET, plan.regions[2].offset);
    TEST_ASSERT_EQUAL_UINT32(C6_FLASH_OTADATA_SIZE, plan.regions[2].size);
    TEST_ASSERT_EQUAL_STRING("updater", plan.regions[3].name);
    TEST_ASSERT_EQUAL(ROM_REGION_WRITE, plan.regions[3].kind);
    TEST_ASSERT_EQUAL_UINT32(C6_FLASH_TEST_OFFSET, plan.regions[3].offset);
    TEST_ASSERT_EQUAL_UINT32(0x30000, plan.regions[3].size);
}

TEST_CASE("On base image of valid sizes: writes no RCP firmware and touches neither RCP slot", "[rcp_sync][rom_plan]")
{
    TEST_ASSERT_EQUAL(ESP_OK, rom_plan_build_base(&plan, blob, C6_FLASH_PTABLE_OFFSET, blob, C6_FLASH_NVS_OFFSET - C6_FLASH_PTABLE_OFFSET, blob,
                                                  C6_FLASH_TEST_SIZE));
    for (size_t i = 0; i < plan.count; i++) {
        TEST_ASSERT_TRUE(plan.regions[i].offset + plan.regions[i].size <= C6_FLASH_OTA0_OFFSET);
    }
}

TEST_CASE("On base image with a blob at its exact limit: is accepted", "[rcp_sync][rom_plan]")
{
    TEST_ASSERT_EQUAL(ESP_OK, rom_plan_build_base(&plan, blob, C6_FLASH_PTABLE_OFFSET, blob, C6_FLASH_NVS_OFFSET - C6_FLASH_PTABLE_OFFSET, blob,
                                                  C6_FLASH_TEST_SIZE));
}

TEST_CASE("On an empty blob: returns ESP_ERR_INVALID_SIZE and plans nothing", "[rcp_sync][rom_plan]")
{
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, rom_plan_build_base(&plan, blob, 0, blob, 0xc00, blob, 0x30000));
    TEST_ASSERT_EQUAL(0, plan.count);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, rom_plan_build_base(&plan, blob, 0x5000, blob, 0, blob, 0x30000));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, rom_plan_build_base(&plan, blob, 0x5000, blob, 0xc00, blob, 0));
    TEST_ASSERT_EQUAL(0, plan.count);
}

TEST_CASE("On a bootloader larger than the room before the partition table: returns ESP_ERR_INVALID_SIZE and plans nothing", "[rcp_sync][rom_plan]")
{
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, rom_plan_build_base(&plan, blob, C6_FLASH_PTABLE_OFFSET + 1, blob, 0xc00, blob, 0x30000));
    TEST_ASSERT_EQUAL(0, plan.count);
}

TEST_CASE("On a partition table larger than the room before nvs: returns ESP_ERR_INVALID_SIZE and plans nothing", "[rcp_sync][rom_plan]")
{
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, rom_plan_build_base(&plan, blob, 0x5000, blob, C6_FLASH_NVS_OFFSET - C6_FLASH_PTABLE_OFFSET + 1, blob,
                                                                0x30000));
    TEST_ASSERT_EQUAL(0, plan.count);
}

TEST_CASE("On an updater larger than the test partition: returns ESP_ERR_INVALID_SIZE and plans nothing", "[rcp_sync][rom_plan]")
{
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, rom_plan_build_base(&plan, blob, 0x5000, blob, 0xc00, blob, C6_FLASH_TEST_SIZE + 1));
    TEST_ASSERT_EQUAL(0, plan.count);
}

TEST_CASE("On a NULL blob or plan: returns ESP_ERR_INVALID_ARG", "[rcp_sync][rom_plan]")
{
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, rom_plan_build_base(&plan, NULL, 0x5000, blob, 0xc00, blob, 0x30000));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, rom_plan_build_base(NULL, blob, 0x5000, blob, 0xc00, blob, 0x30000));
}

TEST_CASE("On an erase plan with aligned offset and size: plans one erase region", "[rcp_sync][rom_plan]")
{
    TEST_ASSERT_EQUAL(ESP_OK, rom_plan_build_erase(&plan, "test", C6_FLASH_TEST_OFFSET, C6_FLASH_TEST_SIZE));
    TEST_ASSERT_EQUAL(1, plan.count);
    TEST_ASSERT_EQUAL(ROM_REGION_ERASE, plan.regions[0].kind);
    TEST_ASSERT_EQUAL_UINT32(C6_FLASH_TEST_OFFSET, plan.regions[0].offset);
    TEST_ASSERT_EQUAL_UINT32(C6_FLASH_TEST_SIZE, plan.regions[0].size);
}

TEST_CASE("On an erase plan with an unaligned or empty range: returns ESP_ERR_INVALID_ARG", "[rcp_sync][rom_plan]")
{
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, rom_plan_build_erase(&plan, "x", 0x20001, 0x1000));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, rom_plan_build_erase(&plan, "x", 0x20000, 0x1001));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, rom_plan_build_erase(&plan, "x", 0x20000, 0));
    TEST_ASSERT_EQUAL(0, plan.count);
}

/* ---- runner ---- */

#define MAX_EVENTS 32

typedef struct {
    esp_err_t connect_result;
    int fail_connect_call;    /* the n-th connect call (0-based) fails; -1: none */
    uint32_t bauds[8];        /* baud argument of each connect call */
    int connect_count;
    int fail_write_count;     /* the next n write_region calls fail */
    int fail_verify_count;    /* the next n verify_region calls fail */
    int fail_verify_region;   /* -1: any region; else only the region with this index */
    char events[MAX_EVENTS][24];
    int event_count;
    int release_count;
} flasher_t;

static flasher_t fl;
static rom_plan_t run_plan;

static void event(const char *what, const char *name)
{
    if (fl.event_count < MAX_EVENTS) {
        snprintf(fl.events[fl.event_count++], sizeof(fl.events[0]), "%s %s", what, name);
    }
}

static int count_events(const char *text)
{
    int n = 0;

    for (int i = 0; i < fl.event_count; i++) {
        n += strcmp(fl.events[i], text) == 0;
    }
    return n;
}

static int region_index(const rom_region_t *r)
{
    return (int)(r - run_plan.regions);
}

static esp_err_t f_connect(uint32_t baud)
{
    event("connect", "");
    if (fl.connect_count < 8) {
        fl.bauds[fl.connect_count] = baud;
    }
    if (fl.connect_count++ == fl.fail_connect_call) {
        return ESP_ERR_TIMEOUT;
    }
    return fl.connect_result;
}

static esp_err_t f_write(const rom_region_t *r)
{
    event("write", r->name);
    if (fl.fail_write_count > 0) {
        fl.fail_write_count--;
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t f_verify(const rom_region_t *r)
{
    event("verify", r->name);
    if (fl.fail_verify_count > 0 && (fl.fail_verify_region < 0 || fl.fail_verify_region == region_index(r))) {
        fl.fail_verify_count--;
        return ESP_ERR_INVALID_CRC;
    }
    return ESP_OK;
}

static void f_release(void)
{
    fl.release_count++;
    event("release", "");
}

static const rom_ops_t ops = {.connect = f_connect, .write_region = f_write, .verify_region = f_verify, .release = f_release};

static void run_setup(void)
{
    memset(&fl, 0, sizeof(fl));
    fl.fail_verify_region = -1;
    fl.fail_connect_call = -1;
    TEST_ASSERT_EQUAL(ESP_OK, rom_plan_build_base(&run_plan, blob, 0x5000, blob, 0xc00, blob, 0x30000));
}

TEST_CASE("On a healthy run: writes and verifies every region in plan order and returns ESP_OK", "[rcp_sync][rom_plan]")
{
    run_setup();
    TEST_ASSERT_EQUAL(ESP_OK, rom_plan_run(&run_plan, &ops));
    TEST_ASSERT_EQUAL(1, count_events("connect "));
    TEST_ASSERT_EQUAL_STRING("write bootloader", fl.events[1]);
    TEST_ASSERT_EQUAL_STRING("verify bootloader", fl.events[2]);
    TEST_ASSERT_EQUAL_STRING("write partition table", fl.events[3]);
    TEST_ASSERT_EQUAL_STRING("verify partition table", fl.events[4]);
    TEST_ASSERT_EQUAL_STRING("write otadata", fl.events[5]);
    TEST_ASSERT_EQUAL_STRING("verify otadata", fl.events[6]);
    TEST_ASSERT_EQUAL_STRING("write updater", fl.events[7]);
    TEST_ASSERT_EQUAL_STRING("verify updater", fl.events[8]);
}

TEST_CASE("On a healthy run: releases the flasher once, last", "[rcp_sync][rom_plan]")
{
    run_setup();
    rom_plan_run(&run_plan, &ops);
    TEST_ASSERT_EQUAL(1, fl.release_count);
    TEST_ASSERT_EQUAL_STRING("release ", fl.events[fl.event_count - 1]);
}

TEST_CASE("On connect failing (no sync or target not an ESP32-C6): returns the error, writes nothing, releases once", "[rcp_sync][rom_plan]")
{
    run_setup();
    fl.connect_result = ESP_ERR_NOT_SUPPORTED;
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_SUPPORTED, rom_plan_run(&run_plan, &ops));
    TEST_ASSERT_EQUAL(0, count_events("write bootloader"));
    TEST_ASSERT_EQUAL(1, fl.release_count);
}

TEST_CASE("On a verification failure that the retry fixes: writes that region twice and succeeds", "[rcp_sync][rom_plan]")
{
    run_setup();
    fl.fail_verify_count = 1;
    TEST_ASSERT_EQUAL(ESP_OK, rom_plan_run(&run_plan, &ops));
    TEST_ASSERT_EQUAL(2, count_events("write bootloader"));
    TEST_ASSERT_EQUAL(2, count_events("verify bootloader"));
    TEST_ASSERT_EQUAL(1, count_events("write updater"));
    TEST_ASSERT_EQUAL(1, fl.release_count);
}

TEST_CASE("On a write failure that the retry fixes: succeeds", "[rcp_sync][rom_plan]")
{
    run_setup();
    fl.fail_write_count = 1;
    TEST_ASSERT_EQUAL(ESP_OK, rom_plan_run(&run_plan, &ops));
    TEST_ASSERT_EQUAL(2, count_events("write bootloader"));
    TEST_ASSERT_EQUAL(1, count_events("verify bootloader"));
}

TEST_CASE("On a verification failing twice: returns the error, stops before the next region, releases once", "[rcp_sync][rom_plan]")
{
    run_setup();
    fl.fail_verify_count = ROM_REGION_ATTEMPTS;
    fl.fail_verify_region = 1; /* partition table */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_CRC, rom_plan_run(&run_plan, &ops));
    TEST_ASSERT_EQUAL(ROM_REGION_ATTEMPTS, count_events("write partition table"));
    TEST_ASSERT_EQUAL(0, count_events("write otadata"));
    TEST_ASSERT_EQUAL(0, count_events("write updater"));
    TEST_ASSERT_EQUAL(1, fl.release_count);
}

TEST_CASE("On a write failing twice: returns the error without verifying and releases once", "[rcp_sync][rom_plan]")
{
    run_setup();
    fl.fail_write_count = ROM_REGION_ATTEMPTS;
    TEST_ASSERT_EQUAL(ESP_FAIL, rom_plan_run(&run_plan, &ops));
    TEST_ASSERT_EQUAL(0, count_events("verify bootloader"));
    TEST_ASSERT_EQUAL(1, fl.release_count);
}

TEST_CASE("On an invalid plan or ops: returns ESP_ERR_INVALID_ARG and connects to nothing", "[rcp_sync][rom_plan]")
{
    run_setup();
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, rom_plan_run(NULL, &ops));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, rom_plan_run(&run_plan, NULL));
    run_plan.count = 0;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, rom_plan_run(&run_plan, &ops));
    TEST_ASSERT_EQUAL(0, fl.event_count);
}

/* ---- baud ladder ---- */

TEST_CASE("On rungs of the baud ladder: 1.5 Mbaud, then 460800, then 115200 for every rung after", "[rcp_sync][rom_plan]")
{
    TEST_ASSERT_EQUAL_UINT32(1500000, rom_plan_baud_for_rung(0));
    TEST_ASSERT_EQUAL_UINT32(460800, rom_plan_baud_for_rung(1));
    TEST_ASSERT_EQUAL_UINT32(115200, rom_plan_baud_for_rung(2));
    TEST_ASSERT_EQUAL_UINT32(115200, rom_plan_baud_for_rung(3));
    TEST_ASSERT_EQUAL_UINT32(115200, rom_plan_baud_for_rung(100));
}

TEST_CASE("On a healthy run: connects once at the top of the ladder", "[rcp_sync][rom_plan]")
{
    run_setup();
    TEST_ASSERT_EQUAL(ESP_OK, rom_plan_run(&run_plan, &ops));
    TEST_ASSERT_EQUAL(1, fl.connect_count);
    TEST_ASSERT_EQUAL_UINT32(1500000, fl.bauds[0]);
}

TEST_CASE("On a failure that the first retry fixes: reconnects at 460800 before writing the region again", "[rcp_sync][rom_plan]")
{
    run_setup();
    fl.fail_verify_count = 1;
    TEST_ASSERT_EQUAL(ESP_OK, rom_plan_run(&run_plan, &ops));
    TEST_ASSERT_EQUAL(2, fl.connect_count);
    TEST_ASSERT_EQUAL_UINT32(460800, fl.bauds[1]);
    /* write, verify (fails), connect, write, verify */
    TEST_ASSERT_EQUAL_STRING("connect ", fl.events[3]);
    TEST_ASSERT_EQUAL_STRING("write bootloader", fl.events[4]);
}

TEST_CASE("On a failure that only the second retry fixes: reconnects at 460800 and then 115200", "[rcp_sync][rom_plan]")
{
    run_setup();
    fl.fail_verify_count = 2;
    TEST_ASSERT_EQUAL(ESP_OK, rom_plan_run(&run_plan, &ops));
    TEST_ASSERT_EQUAL(3, fl.connect_count);
    TEST_ASSERT_EQUAL_UINT32(460800, fl.bauds[1]);
    TEST_ASSERT_EQUAL_UINT32(115200, fl.bauds[2]);
    TEST_ASSERT_EQUAL(3, count_events("write bootloader"));
}

TEST_CASE("On regions after a retry: the lowered baud rate stays, no reconnect without an error", "[rcp_sync][rom_plan]")
{
    run_setup();
    fl.fail_verify_count = 1;
    TEST_ASSERT_EQUAL(ESP_OK, rom_plan_run(&run_plan, &ops));
    TEST_ASSERT_EQUAL(2, fl.connect_count);
    TEST_ASSERT_EQUAL(1, count_events("write updater"));
}

TEST_CASE("On a region failing on every try: runs ROM_REGION_ATTEMPTS tries, reconnects between them only, and stops", "[rcp_sync][rom_plan]")
{
    run_setup();
    fl.fail_verify_count = ROM_REGION_ATTEMPTS;
    fl.fail_verify_region = 0;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_CRC, rom_plan_run(&run_plan, &ops));
    TEST_ASSERT_EQUAL(ROM_REGION_ATTEMPTS, count_events("write bootloader"));
    TEST_ASSERT_EQUAL(ROM_REGION_ATTEMPTS, fl.connect_count); /* first connect + one per retry */
    TEST_ASSERT_EQUAL(0, count_events("write partition table"));
    TEST_ASSERT_EQUAL(1, fl.release_count);
}

TEST_CASE("On a reconnect that fails: returns its error, does not write again and releases once", "[rcp_sync][rom_plan]")
{
    run_setup();
    fl.fail_write_count = 1;
    fl.fail_connect_call = 1;
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, rom_plan_run(&run_plan, &ops));
    TEST_ASSERT_EQUAL(1, count_events("write bootloader"));
    TEST_ASSERT_EQUAL(1, fl.release_count);
}
