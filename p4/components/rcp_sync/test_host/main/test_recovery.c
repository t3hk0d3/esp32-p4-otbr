// Trigger rules of the automatic ROM recovery and the manual entry (spec 5.3 step 3.2, 5.10), on a scripted C6.
#include <stdbool.h>
#include <string.h>
#include "unity.h"
#include "link.h"
#include "rcp_sync_proto.h"

#define BASE_VERSION "base-1a2b3c4d"
#define OLD_VERSION  "base-00000000"
#define RCP_VERSION  "20261001T000000Z-bbbbbbb"

typedef struct {
    /* script */
    bool jumper;
    bool silent;                  /* updater never answers until a recovery fixed it */
    char updater_version[32];
    const char *base;             /* NULL: no embedded base image */
    esp_err_t recover_result;
    /* observations */
    int enter_count;
    int recover_count;
    int release_before_recover;   /* release calls seen when rom_recover ran */
    int release_count;
    bool fixed;
} fake_t;

static fake_t f;

static void setup(bool jumper, bool silent, const char *updater_version)
{
    memset(&f, 0, sizeof(f));
    f.jumper = jumper;
    f.silent = silent;
    strcpy(f.updater_version, updater_version);
    f.base = BASE_VERSION;
    f.recover_result = ESP_OK;
    rcp_sync_recovery_guard_reset();
}

static esp_err_t fake_enter(void)
{
    f.enter_count++;
    return ESP_OK;
}

static void fake_release(void)
{
    f.release_count++;
}

static esp_err_t fake_transact(const uint8_t *req, size_t req_len, uint8_t *reply, size_t cap, size_t *reply_len,
                               uint32_t timeout_ms)
{
    rcp_sync_proto_frame_t in;
    uint8_t payload[RCP_SYNC_PROTO_HELLO_REPLY_LEN];
    size_t n = 0;

    if (f.silent) {
        return ESP_ERR_TIMEOUT;
    }
    TEST_ASSERT_EQUAL(ESP_OK, rcp_sync_proto_decode(req, req_len, &in));
    rcp_sync_hello_reply_t h = {.proto_version = 1, .slot_size = 0x180000};
    strcpy(h.updater_version, f.updater_version);
    strcpy(h.boot_slot_version, RCP_VERSION);
    TEST_ASSERT_EQUAL(ESP_OK, rcp_sync_proto_put_hello_reply(&h, payload, sizeof(payload), &n));
    const rcp_sync_proto_frame_t out = {.type = RCP_SYNC_MSG_OK, .seq = in.seq, .len = (uint16_t)n, .payload = payload};
    return rcp_sync_proto_encode(&out, reply, cap, reply_len);
}

static bool fake_jumper(void)
{
    return f.jumper;
}

static esp_err_t fake_recover(void)
{
    f.recover_count++;
    f.release_before_recover = f.release_count;
    if (f.recover_result == ESP_OK) {
        f.silent = false;
        f.fixed = true;
        strcpy(f.updater_version, BASE_VERSION);
    }
    return f.recover_result;
}

static const char *fake_base(void)
{
    return f.base;
}

static const rcp_sync_link_t test_link = {
    .enter_updater = fake_enter,
    .transact = fake_transact,
    .release = fake_release,
    .jumper_present = fake_jumper,
    .rom_recover = fake_recover,
    .base_version = fake_base,
};

TEST_CASE("On jumper present and updater silent after retries: performs ROM recovery once", "[rcp_sync][recovery]")
{
    rcp_sync_probe_result_t r;

    setup(true, true, BASE_VERSION);
    TEST_ASSERT_EQUAL(ESP_OK, rcp_sync_probe_with(&test_link, &r));
    TEST_ASSERT_EQUAL(1, f.recover_count);
}

TEST_CASE("On jumper present and updater silent after retries: tries HELLO three times before the recovery", "[rcp_sync][recovery]")
{
    rcp_sync_probe_result_t r;

    setup(true, true, BASE_VERSION);
    rcp_sync_probe_with(&test_link, &r);
    /* 3 failing attempts, then 1 successful attempt after the recovery */
    TEST_ASSERT_EQUAL(4, f.enter_count);
}

TEST_CASE("On jumper present and updater silent after retries: the probe afterwards sees the base image version", "[rcp_sync][recovery]")
{
    rcp_sync_probe_result_t r;

    setup(true, true, BASE_VERSION);
    rcp_sync_probe_with(&test_link, &r);
    TEST_ASSERT_TRUE(r.updater_reachable);
    TEST_ASSERT_TRUE(r.rom_recovered);
    TEST_ASSERT_FALSE(r.rom_recovery_failed);
    TEST_ASSERT_EQUAL_STRING(BASE_VERSION, r.updater_version);
    TEST_ASSERT_EQUAL_STRING("", r.last_error);
}

TEST_CASE("On jumper present and updater_version differs from the embedded base version: performs ROM recovery once", "[rcp_sync][recovery]")
{
    rcp_sync_probe_result_t r;

    setup(true, false, OLD_VERSION);
    rcp_sync_probe_with(&test_link, &r);
    TEST_ASSERT_EQUAL(1, f.recover_count);
    TEST_ASSERT_EQUAL_STRING(BASE_VERSION, r.updater_version);
    TEST_ASSERT_EQUAL_STRING(RCP_VERSION, r.installed_version);
}

TEST_CASE("On jumper present and updater_version equal to the embedded base version: is not attempted", "[rcp_sync][recovery]")
{
    rcp_sync_probe_result_t r;

    setup(true, false, BASE_VERSION);
    rcp_sync_probe_with(&test_link, &r);
    TEST_ASSERT_EQUAL(0, f.recover_count);
    TEST_ASSERT_TRUE(r.jumper_present);
    TEST_ASSERT_FALSE(r.rom_recovered);
}

TEST_CASE("On jumper absent and updater silent: is not attempted", "[rcp_sync][recovery]")
{
    rcp_sync_probe_result_t r;

    setup(false, true, BASE_VERSION);
    TEST_ASSERT_EQUAL(ESP_OK, rcp_sync_probe_with(&test_link, &r));
    TEST_ASSERT_EQUAL(0, f.recover_count);
    TEST_ASSERT_FALSE(r.updater_reachable);
    TEST_ASSERT_FALSE(r.jumper_present);
}

TEST_CASE("On jumper absent and updater_version differs: is not attempted", "[rcp_sync][recovery]")
{
    rcp_sync_probe_result_t r;

    setup(false, false, OLD_VERSION);
    rcp_sync_probe_with(&test_link, &r);
    TEST_ASSERT_EQUAL(0, f.recover_count);
    TEST_ASSERT_EQUAL_STRING(OLD_VERSION, r.updater_version);
}

TEST_CASE("On a link without jumper support: is not attempted", "[rcp_sync][recovery]")
{
    rcp_sync_probe_result_t r;
    rcp_sync_link_t bare = test_link;

    setup(true, true, BASE_VERSION);
    bare.jumper_present = NULL;
    bare.rom_recover = NULL;
    TEST_ASSERT_EQUAL(ESP_OK, rcp_sync_probe_with(&bare, &r));
    TEST_ASSERT_EQUAL(0, f.recover_count);
    TEST_ASSERT_FALSE(r.jumper_present);
}

TEST_CASE("On second trigger within the same boot (automatic): is not attempted", "[rcp_sync][recovery]")
{
    rcp_sync_probe_result_t r;

    setup(true, true, BASE_VERSION);
    rcp_sync_probe_with(&test_link, &r);
    TEST_ASSERT_EQUAL(1, f.recover_count);
    f.silent = true; /* the updater is gone again */
    rcp_sync_probe_with(&test_link, &r);
    TEST_ASSERT_EQUAL(1, f.recover_count);
    TEST_ASSERT_FALSE(r.updater_reachable);
    TEST_ASSERT_FALSE(r.rom_recovered);
}

TEST_CASE("On a failed recovery: reports it, makes no second attempt in the probe and none in the next one", "[rcp_sync][recovery]")
{
    rcp_sync_probe_result_t r;

    setup(true, true, BASE_VERSION);
    f.recover_result = ESP_FAIL;
    TEST_ASSERT_EQUAL(ESP_OK, rcp_sync_probe_with(&test_link, &r));
    TEST_ASSERT_EQUAL(1, f.recover_count);
    TEST_ASSERT_TRUE(r.rom_recovery_failed);
    TEST_ASSERT_FALSE(r.rom_recovered);
    TEST_ASSERT_FALSE(r.updater_reachable);
    TEST_ASSERT_TRUE(strlen(r.last_error) > 0);
    rcp_sync_probe_with(&test_link, &r);
    TEST_ASSERT_EQUAL(1, f.recover_count);
}

TEST_CASE("On a failed recovery that cannot repair the updater: the probe does not loop", "[rcp_sync][recovery]")
{
    rcp_sync_probe_result_t r;

    setup(true, true, BASE_VERSION);
    f.recover_result = ESP_FAIL;
    rcp_sync_probe_with(&test_link, &r);
    TEST_ASSERT_EQUAL(3, f.enter_count);
}

TEST_CASE("On no embedded base image: is not attempted", "[rcp_sync][recovery]")
{
    rcp_sync_probe_result_t r;

    setup(true, true, BASE_VERSION);
    f.base = NULL;
    rcp_sync_probe_with(&test_link, &r);
    TEST_ASSERT_EQUAL(0, f.recover_count);
}

TEST_CASE("On ROM recovery: the link is released before the flasher takes the pins", "[rcp_sync][recovery]")
{
    rcp_sync_probe_result_t r;

    setup(true, true, BASE_VERSION);
    rcp_sync_probe_with(&test_link, &r);
    TEST_ASSERT_TRUE(f.release_before_recover >= 1);
}

TEST_CASE("On manual ROM recovery: runs although an automatic one ran this boot", "[rcp_sync][recovery]")
{
    rcp_sync_probe_result_t r;

    setup(true, true, BASE_VERSION);
    rcp_sync_probe_with(&test_link, &r);
    TEST_ASSERT_EQUAL(ESP_OK, rcp_sync_rom_recover_with(&test_link));
    TEST_ASSERT_EQUAL(2, f.recover_count);
}

TEST_CASE("On manual ROM recovery failing: returns the error", "[rcp_sync][recovery]")
{
    setup(true, false, BASE_VERSION);
    f.recover_result = ESP_ERR_INVALID_CRC;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_CRC, rcp_sync_rom_recover_with(&test_link));
}

TEST_CASE("On manual ROM recovery without a recover function on the link: returns ESP_ERR_NOT_SUPPORTED", "[rcp_sync][recovery]")
{
    rcp_sync_link_t bare = test_link;

    setup(true, false, BASE_VERSION);
    bare.rom_recover = NULL;
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_SUPPORTED, rcp_sync_rom_recover_with(&bare));
}

TEST_CASE("On a failed recovery triggered by a version mismatch: the probe still reports the reachable updater and its installed RCP", "[rcp_sync][recovery]")
{
    rcp_sync_probe_result_t r;

    setup(true, false, OLD_VERSION);
    f.recover_result = ESP_FAIL;
    rcp_sync_probe_with(&test_link, &r);
    TEST_ASSERT_TRUE(r.rom_recovery_failed);
    TEST_ASSERT_TRUE(r.updater_reachable);
    TEST_ASSERT_EQUAL_STRING(OLD_VERSION, r.updater_version);
    TEST_ASSERT_EQUAL_STRING(RCP_VERSION, r.installed_version);
    TEST_ASSERT_TRUE(r.installed_valid);
    TEST_ASSERT_EQUAL_STRING("", r.last_error);
}
