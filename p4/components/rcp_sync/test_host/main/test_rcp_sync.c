#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "unity.h"
#include "unity_test_runner.h"
#include "link.h"
#include "rcp_sync_proto.h"

#define IMAGE_SIZE 10000 /* three DATA chunks */
#define SLOT_SIZE (2 * IMAGE_SIZE)
#define BOOT_VERSION "20261001T000000Z-bbbbbbb"

/* ---- scripted C6 updater ---- */

typedef struct {
    /* script */
    char boot_version[32];
    bool silent;                /* never answers (updater not running) */
    int nak_data_offset;        /* -1: never; else the first DATA at this offset is answered with NAK */
    int reset_at_offset;        /* -1: never; else the C6 resets (no reply) on the first DATA at this offset */
    bool fail_begin;            /* BEGIN always answered with ERR(FLASH) */
    bool verify_error;          /* END answered with ERR(VERIFY) */
    int fail_flash_at_offset;   /* > 0: DATA at this offset answered with ERR(FLASH) */
    bool reject_first_chunk;    /* the first DATA chunk answered with ERR(FLASH), as esp_ota_write does for a bad header */
    /* state */
    bool receiving;
    uint32_t next_offset;
    uint8_t flash[SLOT_SIZE];
    /* observations */
    int enter_count, release_count, begin_count, end_count;
    uint32_t begin_size;
    uint32_t data_offsets[16];
    int data_count;
    bool nak_done, nak_pending_compare, resent_identical;
    uint8_t nak_req[RCP_SYNC_PROTO_HEADER_LEN + RCP_SYNC_PROTO_MAX_PAYLOAD + RCP_SYNC_PROTO_CRC_LEN];
    size_t nak_req_len;
    uint8_t nak_seq, seq_after_nak;
    bool reset_done;
    bool gpio6_high;
} fake_c6_t;

static fake_c6_t c6;
static uint8_t image[IMAGE_SIZE];
static uint8_t seqs[64];
static int seq_count;

static void c6_setup(const char *boot_version)
{
    memset(&c6, 0, sizeof(c6));
    strcpy(c6.boot_version, boot_version);
    c6.nak_data_offset = -1;
    c6.reset_at_offset = -1;
    for (int i = 0; i < IMAGE_SIZE; i++) {
        image[i] = (uint8_t)(i * 7 + 3);
    }
    seq_count = 0;
}

static esp_err_t reply_frame(uint8_t type, uint8_t seq, const uint8_t *payload, uint16_t len, uint8_t *out, size_t *out_len)
{
    const rcp_sync_proto_frame_t f = {.type = type, .seq = seq, .len = len, .payload = payload};
    return rcp_sync_proto_encode(&f, out, 128, out_len);
}

static void u32(uint8_t *p, uint32_t v)
{
    memcpy(p, &v, 4); /* host is little-endian */
}

static esp_err_t err_reply(uint8_t seq, uint16_t code, uint8_t *reply, size_t *reply_len)
{
    uint8_t p[6] = {(uint8_t)code, (uint8_t)(code >> 8), 0, 0, 0, 0};
    return reply_frame(RCP_SYNC_MSG_ERR, seq, p, sizeof(p), reply, reply_len);
}

static esp_err_t fake_enter(void)
{
    c6.enter_count++;
    c6.receiving = false;
    c6.gpio6_high = false;
    return ESP_OK;
}

static void fake_release(void)
{
    c6.release_count++;
    c6.gpio6_high = true;
}

static esp_err_t fake_transact(const uint8_t *req, size_t req_len, uint8_t *reply, size_t cap, size_t *reply_len,
                               uint32_t timeout_ms)
{
    rcp_sync_proto_frame_t f;
    uint8_t p[RCP_SYNC_PROTO_HELLO_REPLY_LEN];
    size_t n = 0;

    if (c6.silent) {
        return ESP_ERR_TIMEOUT;
    }
    TEST_ASSERT_EQUAL(ESP_OK, rcp_sync_proto_decode(req, req_len, &f));
    TEST_ASSERT_TRUE(cap >= 112);
    if (seq_count < 64) {
        seqs[seq_count++] = f.seq;
    }
    if (c6.nak_pending_compare) {
        c6.nak_pending_compare = false;
        c6.seq_after_nak = f.seq;
        c6.resent_identical = req_len == c6.nak_req_len && memcmp(req, c6.nak_req, req_len) == 0;
    }

    switch (f.type) {
    case RCP_SYNC_MSG_HELLO: {
        rcp_sync_hello_reply_t h = {.proto_version = 1, .slot_size = SLOT_SIZE};
        strcpy(h.updater_version, "updater-1");
        strcpy(h.boot_slot_version, c6.boot_version);
        TEST_ASSERT_EQUAL(ESP_OK, rcp_sync_proto_put_hello_reply(&h, p, sizeof(p), &n));
        return reply_frame(RCP_SYNC_MSG_OK, f.seq, p, (uint16_t)n, reply, reply_len);
    }
    case RCP_SYNC_MSG_BEGIN:
        c6.begin_count++;
        memcpy(&c6.begin_size, f.payload, 4);
        if (c6.fail_begin) {
            return err_reply(f.seq, RCP_SYNC_ERR_FLASH, reply, reply_len);
        }
        c6.receiving = true;
        c6.next_offset = 0;
        return reply_frame(RCP_SYNC_MSG_OK, f.seq, NULL, 0, reply, reply_len);
    case RCP_SYNC_MSG_DATA: {
        uint32_t off;
        memcpy(&off, f.payload, 4);
        if (c6.nak_data_offset >= 0 && !c6.nak_done && (int)off == c6.nak_data_offset) {
            c6.nak_done = true;
            c6.nak_pending_compare = true;
            c6.nak_seq = f.seq;
            c6.nak_req_len = req_len;
            memcpy(c6.nak_req, req, req_len);
            return reply_frame(RCP_SYNC_MSG_NAK, 0, NULL, 0, reply, reply_len);
        }
        if (c6.reset_at_offset >= 0 && !c6.reset_done && (int)off == c6.reset_at_offset) {
            c6.reset_done = true;
            c6.receiving = false; /* the C6 rebooted into a fresh updater */
            return ESP_ERR_TIMEOUT;
        }
        if (!c6.receiving || off != c6.next_offset) {
            return err_reply(f.seq, RCP_SYNC_ERR_STATE, reply, reply_len);
        }
        if (c6.fail_flash_at_offset > 0 && (int)off == c6.fail_flash_at_offset) {
            return err_reply(f.seq, RCP_SYNC_ERR_FLASH, reply, reply_len);
        }
        if (c6.reject_first_chunk && off == 0) {
            return err_reply(f.seq, RCP_SYNC_ERR_FLASH, reply, reply_len);
        }
        if (c6.data_count < 16) {
            c6.data_offsets[c6.data_count++] = off;
        }
        memcpy(&c6.flash[off], f.payload + 4, f.len - 4u);
        c6.next_offset += f.len - 4u;
        u32(p, c6.next_offset);
        return reply_frame(RCP_SYNC_MSG_OK, f.seq, p, 4, reply, reply_len);
    }
    case RCP_SYNC_MSG_END:
        c6.end_count++;
        if (c6.verify_error) {
            return err_reply(f.seq, RCP_SYNC_ERR_VERIFY, reply, reply_len);
        }
        return reply_frame(RCP_SYNC_MSG_OK, f.seq, NULL, 0, reply, reply_len);
    default:
        return ESP_ERR_INVALID_RESPONSE;
    }
}

static const rcp_sync_link_t fake_link = {.enter_updater = fake_enter, .transact = fake_transact, .release = fake_release};

/* ---- fake firmware source ---- */

static size_t src_next_offset; /* sequential read check: offsets ascend without gaps */
static int src_reopen_count;
static bool src_sequential;
static int src_read_count;

static esp_err_t src_read(void *ctx, size_t offset, void *buf, size_t len)
{
    src_read_count++;
    if (offset != src_next_offset) {
        src_sequential = false;
    }
    src_next_offset = offset + len;
    memcpy(buf, &image[offset], len);
    return ESP_OK;
}

static esp_err_t src_reopen(void *ctx)
{
    src_reopen_count++;
    src_next_offset = 0;
    return ESP_OK;
}

static rcp_fw_source_t source(bool reopenable, size_t total_size)
{
    src_next_offset = 0;
    src_reopen_count = 0;
    src_read_count = 0;
    src_sequential = true;
    return (rcp_fw_source_t) {
        .total_size = total_size, .read = src_read, .reopen = reopenable ? src_reopen : NULL
    };
}

static char detail[64];

static esp_err_t push(const rcp_fw_source_t *src)
{
    detail[0] = 'x';
    return rcp_sync_push_firmware_with(&fake_link, src, detail, sizeof(detail));
}

static rcp_sync_probe_result_t probe(void)
{
    rcp_sync_probe_result_t r;
    TEST_ASSERT_EQUAL(ESP_OK, rcp_sync_probe_with(&fake_link, &r));
    return r;
}

/* ---- rcp_sync_probe: one test per BDD effect ---- */

TEST_CASE("On updater answering HELLO with a boot-slot version: returns it as installed_version with installed_valid true", "[rcp_sync]")
{
    c6_setup(BOOT_VERSION);
    rcp_sync_probe_result_t r = probe();
    TEST_ASSERT_TRUE(r.updater_reachable);
    TEST_ASSERT_TRUE(r.installed_valid);
    TEST_ASSERT_EQUAL_STRING(BOOT_VERSION, r.installed_version);
    TEST_ASSERT_EQUAL_STRING("updater-1", r.updater_version);
}

TEST_CASE("On updater answering HELLO with a boot-slot version: rcp_sync_get_last_probe returns the same result", "[rcp_sync]")
{
    c6_setup(BOOT_VERSION);
    (void)probe();
    rcp_sync_probe_result_t last;
    rcp_sync_get_last_probe(&last);
    TEST_ASSERT_EQUAL_STRING(BOOT_VERSION, last.installed_version);
}

TEST_CASE("On a later probe: a copy taken earlier keeps the earlier result", "[rcp_sync]")
{
    c6_setup(BOOT_VERSION);
    (void)probe();
    rcp_sync_probe_result_t earlier;
    rcp_sync_get_last_probe(&earlier);
    c6_setup("other-version");
    (void)probe();
    TEST_ASSERT_EQUAL_STRING(BOOT_VERSION, earlier.installed_version);
}

TEST_CASE("On a later probe: rcp_sync_get_last_probe returns the later result", "[rcp_sync]")
{
    c6_setup(BOOT_VERSION);
    (void)probe();
    c6_setup("other-version");
    (void)probe();
    rcp_sync_probe_result_t last;
    rcp_sync_get_last_probe(&last);
    TEST_ASSERT_EQUAL_STRING("other-version", last.installed_version);
}

TEST_CASE("On updater answering HELLO with a boot-slot version: sends no BEGIN", "[rcp_sync]")
{
    c6_setup(BOOT_VERSION);
    probe();
    TEST_ASSERT_EQUAL(0, c6.begin_count);
}

TEST_CASE("On updater answering HELLO with an empty boot-slot version: returns installed_valid false and an empty installed_version", "[rcp_sync]")
{
    c6_setup("");
    rcp_sync_probe_result_t r = probe();
    TEST_ASSERT_TRUE(r.updater_reachable);
    TEST_ASSERT_FALSE(r.installed_valid);
    TEST_ASSERT_EQUAL_STRING("", r.installed_version);
}

TEST_CASE("On updater not answering after the normal retries: returns ESP_OK with updater_reachable false", "[rcp_sync]")
{
    c6_setup(BOOT_VERSION);
    c6.silent = true;
    rcp_sync_probe_result_t r;
    TEST_ASSERT_EQUAL(ESP_OK, rcp_sync_probe_with(&fake_link, &r));
    TEST_ASSERT_FALSE(r.updater_reachable);
    TEST_ASSERT_FALSE(r.installed_valid);
    TEST_ASSERT_EQUAL(3, c6.enter_count);
}

TEST_CASE("On updater not answering after the normal retries: last_error is set", "[rcp_sync]")
{
    c6_setup(BOOT_VERSION);
    c6.silent = true;
    rcp_sync_probe_result_t r = probe();
    TEST_ASSERT_NOT_EQUAL(0, r.last_error[0]);
    rcp_sync_probe_result_t last;
    rcp_sync_get_last_probe(&last);
    TEST_ASSERT_EQUAL_STRING(r.last_error, last.last_error);
}

TEST_CASE("On probe completion (any result): releases the SPI bus", "[rcp_sync]")
{
    c6_setup(BOOT_VERSION);
    probe();
    TEST_ASSERT_EQUAL(1, c6.release_count);
    c6_setup(BOOT_VERSION);
    c6.silent = true;
    probe();
    TEST_ASSERT_EQUAL(1, c6.release_count);
}

TEST_CASE("On probe completion (any result): drives GPIO6 high", "[rcp_sync]")
{
    c6_setup(BOOT_VERSION);
    probe();
    TEST_ASSERT_TRUE(c6.gpio6_high);
    c6_setup(BOOT_VERSION);
    c6.silent = true;
    probe();
    TEST_ASSERT_TRUE(c6.gpio6_high);
}

/* ---- rcp_sync_push_firmware: one test per BDD effect ---- */

TEST_CASE("On source of valid size and a healthy transfer: sends BEGIN with the total size, DATA at ascending offsets and END once", "[rcp_sync]")
{
    c6_setup(BOOT_VERSION);
    rcp_fw_source_t src = source(false, IMAGE_SIZE);
    push(&src);
    TEST_ASSERT_EQUAL(1, c6.begin_count);
    TEST_ASSERT_EQUAL_UINT32(IMAGE_SIZE, c6.begin_size);
    TEST_ASSERT_EQUAL(3, c6.data_count); /* three DATA chunks */
    TEST_ASSERT_EQUAL_UINT32(0, c6.data_offsets[0]);
    TEST_ASSERT_TRUE(c6.data_offsets[1] > c6.data_offsets[0]);
    TEST_ASSERT_TRUE(c6.data_offsets[2] > c6.data_offsets[1]);
    TEST_ASSERT_EQUAL(1, c6.end_count);
    TEST_ASSERT_EQUAL_MEMORY(image, c6.flash, IMAGE_SIZE);
    TEST_ASSERT_TRUE(src_sequential);
}

TEST_CASE("On source of valid size and a healthy transfer: returns ESP_OK", "[rcp_sync]")
{
    c6_setup(BOOT_VERSION);
    rcp_fw_source_t src = source(false, IMAGE_SIZE);
    TEST_ASSERT_EQUAL(ESP_OK, push(&src));
    TEST_ASSERT_EQUAL_STRING("", detail);
}

TEST_CASE("On a frame NAK: resends the same seq with the same bytes", "[rcp_sync]")
{
    c6_setup(BOOT_VERSION);
    c6.nak_data_offset = 4092; /* second chunk */
    rcp_fw_source_t src = source(false, IMAGE_SIZE);
    push(&src);
    TEST_ASSERT_TRUE(c6.nak_done);
    TEST_ASSERT_EQUAL_UINT8(c6.nak_seq, c6.seq_after_nak);
    TEST_ASSERT_TRUE(c6.resent_identical);
}

TEST_CASE("On a frame NAK: completes without re-reading the source", "[rcp_sync]")
{
    c6_setup(BOOT_VERSION);
    c6.nak_data_offset = 4092;
    rcp_fw_source_t src = source(false, IMAGE_SIZE);
    TEST_ASSERT_EQUAL(ESP_OK, push(&src));
    TEST_ASSERT_EQUAL(1, c6.begin_count);
    TEST_ASSERT_EQUAL_MEMORY(image, c6.flash, IMAGE_SIZE);
    TEST_ASSERT_TRUE(src_sequential);
    TEST_ASSERT_EQUAL(3, src_read_count);
}

TEST_CASE("On source size 0 or above slot_size: sends no BEGIN", "[rcp_sync]")
{
    c6_setup(BOOT_VERSION);
    rcp_fw_source_t zero = source(false, 0);
    push(&zero);
    rcp_fw_source_t big = source(false, SLOT_SIZE + 1);
    push(&big);
    TEST_ASSERT_EQUAL(0, c6.begin_count);
    TEST_ASSERT_EQUAL(0, src_read_count);
}

TEST_CASE("On source size 0 or above slot_size: returns ESP_ERR_INVALID_SIZE", "[rcp_sync]")
{
    c6_setup(BOOT_VERSION);
    rcp_fw_source_t zero = source(true, 0);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, push(&zero));
    rcp_fw_source_t big = source(true, SLOT_SIZE + 1);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, push(&big));
    TEST_ASSERT_EQUAL(0, src_reopen_count); /* a bad size is not retried */
}

TEST_CASE("On C6 reset mid-DATA with a one-shot source: makes no whole-transfer retry (no second BEGIN)", "[rcp_sync]")
{
    c6_setup(BOOT_VERSION);
    c6.reset_at_offset = 4092;
    rcp_fw_source_t src = source(false, IMAGE_SIZE);
    push(&src);
    TEST_ASSERT_EQUAL(1, c6.begin_count);
    TEST_ASSERT_EQUAL(1, c6.enter_count);
}

TEST_CASE("On C6 reset mid-DATA with a one-shot source: returns an error with a non-empty detail", "[rcp_sync]")
{
    c6_setup(BOOT_VERSION);
    c6.reset_at_offset = 4092;
    rcp_fw_source_t src = source(false, IMAGE_SIZE);
    TEST_ASSERT_NOT_EQUAL(ESP_OK, push(&src));
    TEST_ASSERT_NOT_EQUAL(0, detail[0]);
    TEST_ASSERT_NOT_EQUAL('x', detail[0]);
}

TEST_CASE("On C6 reset mid-DATA with a one-shot source: sent no END", "[rcp_sync]")
{
    c6_setup(BOOT_VERSION);
    c6.reset_at_offset = 4092;
    rcp_fw_source_t src = source(false, IMAGE_SIZE);
    push(&src);
    TEST_ASSERT_EQUAL(0, c6.end_count);
}

TEST_CASE("On C6 reset mid-DATA with a re-openable source: calls reopen and retries from BEGIN", "[rcp_sync]")
{
    c6_setup(BOOT_VERSION);
    c6.reset_at_offset = 4092;
    rcp_fw_source_t src = source(true, IMAGE_SIZE);
    push(&src);
    TEST_ASSERT_EQUAL(1, src_reopen_count);
    TEST_ASSERT_EQUAL(2, c6.begin_count);
    TEST_ASSERT_EQUAL(2, c6.enter_count);
    TEST_ASSERT_TRUE(src_sequential);
}

TEST_CASE("On C6 reset mid-DATA with a re-openable source: returns ESP_OK if the retry completes", "[rcp_sync]")
{
    c6_setup(BOOT_VERSION);
    c6.reset_at_offset = 4092;
    rcp_fw_source_t src = source(true, IMAGE_SIZE);
    TEST_ASSERT_EQUAL(ESP_OK, push(&src));
    TEST_ASSERT_EQUAL(1, c6.end_count);
    TEST_ASSERT_EQUAL_MEMORY(image, c6.flash, IMAGE_SIZE);
}

TEST_CASE("On C6 failing on all 3 attempts with a re-openable source: makes exactly 3 BEGIN attempts and returns an error", "[rcp_sync]")
{
    c6_setup(BOOT_VERSION);
    c6.fail_begin = true;
    rcp_fw_source_t src = source(true, IMAGE_SIZE);
    TEST_ASSERT_NOT_EQUAL(ESP_OK, push(&src));
    TEST_ASSERT_EQUAL(3, c6.begin_count);
    TEST_ASSERT_EQUAL(2, src_reopen_count);
}

TEST_CASE("On END replying ERR(verify): returns an error", "[rcp_sync]")
{
    c6_setup(BOOT_VERSION);
    c6.verify_error = true;
    rcp_fw_source_t src = source(true, IMAGE_SIZE);
    TEST_ASSERT_EQUAL(RCP_SYNC_ERR_IMAGE_REJECTED, push(&src));
    TEST_ASSERT_NOT_EQUAL(0, detail[0]);
}

TEST_CASE("On the first DATA chunk refused with ERR(flash) (the C6 rejects the image header): returns image rejected without a retry", "[rcp_sync]")
{
    c6_setup(BOOT_VERSION);
    c6.reject_first_chunk = true;
    rcp_fw_source_t src = source(true, IMAGE_SIZE);
    TEST_ASSERT_EQUAL(RCP_SYNC_ERR_IMAGE_REJECTED, push(&src));
    TEST_ASSERT_EQUAL(1, c6.begin_count);
}

TEST_CASE("On ERR(flash) on a later chunk: is a transfer error, not image rejected", "[rcp_sync]")
{
    c6_setup(BOOT_VERSION);
    c6.fail_flash_at_offset = RCP_SYNC_PROTO_MAX_PAYLOAD - 4; /* the second chunk */
    rcp_fw_source_t src = source(true, IMAGE_SIZE);
    const esp_err_t err = push(&src);
    TEST_ASSERT_NOT_EQUAL(ESP_OK, err);
    TEST_ASSERT_NOT_EQUAL(RCP_SYNC_ERR_IMAGE_REJECTED, err);
}

TEST_CASE("On END replying ERR(verify): makes no further attempt", "[rcp_sync]")
{
    c6_setup(BOOT_VERSION);
    c6.verify_error = true;
    rcp_fw_source_t src = source(true, IMAGE_SIZE);
    push(&src);
    TEST_ASSERT_EQUAL(1, c6.begin_count);
    TEST_ASSERT_EQUAL(1, c6.end_count);
    TEST_ASSERT_EQUAL(0, src_reopen_count);
}

TEST_CASE("On push completion (any result): releases the SPI bus", "[rcp_sync]")
{
    rcp_fw_source_t src = source(false, IMAGE_SIZE);
    c6_setup(BOOT_VERSION);
    push(&src);
    TEST_ASSERT_EQUAL(1, c6.release_count);
    c6_setup(BOOT_VERSION);
    c6.fail_begin = true;
    push(&src);
    TEST_ASSERT_EQUAL(1, c6.release_count);
    c6_setup(BOOT_VERSION);
    rcp_fw_source_t zero = source(false, 0);
    push(&zero);
    TEST_ASSERT_EQUAL(1, c6.release_count);
}

TEST_CASE("On push completion (any result): drives GPIO6 high", "[rcp_sync]")
{
    rcp_fw_source_t src = source(false, IMAGE_SIZE);
    c6_setup(BOOT_VERSION);
    push(&src);
    TEST_ASSERT_TRUE(c6.gpio6_high);
    c6_setup(BOOT_VERSION);
    c6.fail_begin = true;
    push(&src);
    TEST_ASSERT_TRUE(c6.gpio6_high);
}

TEST_CASE("On a sequence of requests: each new request uses a fresh distinct seq", "[rcp_sync]")
{
    c6_setup(BOOT_VERSION);
    rcp_fw_source_t src = source(false, IMAGE_SIZE);
    push(&src);
    TEST_ASSERT_TRUE(seq_count >= 6); /* HELLO, BEGIN, 3 DATA, END */
    for (int i = 1; i < seq_count; i++) {
        TEST_ASSERT_NOT_EQUAL(seqs[i - 1], seqs[i]);
    }
}

/* ---- first install through the factory ESP-Hosted firmware ---- */

#define BASE_VERSION "base-12345678"

static struct {
    bool present;    /* ESP-Hosted answers on SDIO */
    bool jumper;
    bool comes_back; /* the updater answers after the OTA */
    esp_err_t ota_err;
    int detect_count, ota_count, attach_count;
    uint8_t got[IMAGE_SIZE];
    size_t got_len;
} hs;

static void hosted_setup(bool esp_hosted)
{
    c6_setup(BOOT_VERSION);
    memset(&hs, 0, sizeof(hs));
    c6.silent = esp_hosted; /* no updater on a factory C6 */
    hs.present = esp_hosted;
    hs.comes_back = true;
}

/* Make `image` an app image of this project and version (esp_image_header_t, segment header, esp_app_desc_t). */
static void make_image(const char *project, const char *version)
{
    memset(image, 0, 128);
    image[0] = 0xE9;
    u32(image + 32, 0xABCD5432);
    strcpy((char *)image + 32 + 16, version);
    strcpy((char *)image + 32 + 48, project);
}

static esp_err_t hs_detect(bool *present)
{
    hs.detect_count++;
    *present = hs.present;
    return ESP_OK;
}

static esp_err_t hs_ota(const rcp_fw_source_t *src, char *d, size_t cap)
{
    hs.ota_count++;
    for (size_t off = 0; off < src->total_size; off += 1400) {
        const size_t n = src->total_size - off < 1400 ? src->total_size - off : 1400;
        TEST_ASSERT_EQUAL(ESP_OK, src->read(src->ctx, off, hs.got + off, n));
        hs.got_len = off + n;
    }
    if (hs.ota_err != ESP_OK) {
        strlcpy(d, "OTAWrite refused (-1)", cap);
        return hs.ota_err;
    }
    c6.silent = !hs.comes_back;
    strcpy(c6.boot_version, ""); /* the installer leaves ota_0 blank */
    return ESP_OK;
}

static esp_err_t hs_attach(void)
{
    hs.attach_count++;
    c6.gpio6_high = false;
    return ESP_OK;
}

static bool hs_jumper(void)
{
    return hs.jumper;
}

static const char *hs_base(void)
{
    return BASE_VERSION;
}

static const rcp_sync_link_t hosted_link = {
    .enter_updater = fake_enter, .transact = fake_transact, .release = fake_release, .jumper_present = hs_jumper,
    .base_version = hs_base, .hosted_detect = hs_detect, .hosted_ota = hs_ota, .attach_updater = hs_attach,
};

static esp_err_t hosted_push(void)
{
    rcp_fw_source_t src = source(false, IMAGE_SIZE);
    detail[0] = 'x';
    return rcp_sync_push_firmware_with(&hosted_link, &src, detail, sizeof(detail));
}

static rcp_sync_probe_result_t hosted_probe(void)
{
    rcp_sync_probe_result_t r;
    TEST_ASSERT_EQUAL(ESP_OK, rcp_sync_probe_with(&hosted_link, &r));
    return r;
}

TEST_CASE("On probe with the updater silent, no jumper, ESP-Hosted answering: esp_hosted and last_error esp-hosted firmware", "[hosted]")
{
    hosted_setup(true);
    rcp_sync_probe_result_t r = hosted_probe();
    TEST_ASSERT_TRUE(r.esp_hosted);
    TEST_ASSERT_EQUAL_STRING(RCP_SYNC_ESP_HOSTED_ERROR, r.last_error);
    TEST_ASSERT_EQUAL(0, hs.ota_count);
    TEST_ASSERT_EQUAL(0, c6.begin_count);
}

TEST_CASE("On probe with the updater silent and nothing on SDIO: updater not responding as before", "[hosted]")
{
    hosted_setup(true);
    hs.present = false;
    rcp_sync_probe_result_t r = hosted_probe();
    TEST_ASSERT_FALSE(r.esp_hosted);
    TEST_ASSERT_EQUAL_STRING("updater not responding", r.last_error);
}

TEST_CASE("On probe with the jumper present: ESP-Hosted is never looked for", "[hosted]")
{
    hosted_setup(true);
    hs.jumper = true;
    rcp_sync_probe_result_t r = hosted_probe();
    TEST_ASSERT_FALSE(r.esp_hosted);
    TEST_ASSERT_EQUAL(0, hs.detect_count);
}

TEST_CASE("On probe with the updater answering: ESP-Hosted is never looked for", "[hosted]")
{
    hosted_setup(false);
    rcp_sync_probe_result_t r = hosted_probe();
    TEST_ASSERT_TRUE(r.updater_reachable);
    TEST_ASSERT_EQUAL(0, hs.detect_count);
}

TEST_CASE("On push to ESP-Hosted of a matching c6_installer: the whole image goes through the OTA unchanged", "[hosted]")
{
    hosted_setup(true);
    make_image(RCP_SYNC_INSTALLER_PROJECT, BASE_VERSION);
    TEST_ASSERT_EQUAL(ESP_OK, hosted_push());
    TEST_ASSERT_EQUAL(1, hs.ota_count);
    TEST_ASSERT_EQUAL(IMAGE_SIZE, hs.got_len);
    TEST_ASSERT_EQUAL_MEMORY(image, hs.got, IMAGE_SIZE);
    TEST_ASSERT_TRUE(src_sequential);
}

TEST_CASE("On push to ESP-Hosted of a matching c6_installer: waits for the updater without a reset and reports the installer", "[hosted]")
{
    hosted_setup(true);
    make_image(RCP_SYNC_INSTALLER_PROJECT, BASE_VERSION);
    TEST_ASSERT_EQUAL(ESP_OK, hosted_push());
    TEST_ASSERT_EQUAL(1, hs.attach_count);
    TEST_ASSERT_EQUAL(1, c6.enter_count); /* only the first HELLO attempt reset the C6 */
    TEST_ASSERT_EQUAL(0, c6.begin_count);
    TEST_ASSERT_TRUE(rcp_sync_last_push_was_installer());
    TEST_ASSERT_TRUE(c6.gpio6_high);
}

TEST_CASE("On push to ESP-Hosted of a c6_installer of another release: image rejected naming both versions, nothing sent", "[hosted]")
{
    hosted_setup(true);
    make_image(RCP_SYNC_INSTALLER_PROJECT, "base-87654321");
    TEST_ASSERT_EQUAL(RCP_SYNC_ERR_IMAGE_REJECTED, hosted_push());
    TEST_ASSERT_NOT_NULL(strstr(detail, "base-87654321"));
    TEST_ASSERT_NOT_NULL(strstr(detail, BASE_VERSION));
    TEST_ASSERT_EQUAL(0, hs.ota_count);
}

TEST_CASE("On push to ESP-Hosted of any other image: image rejected asking for c6_installer.bin, nothing sent", "[hosted]")
{
    hosted_setup(true);
    make_image("c6_rcp", "20261101T000000Z-aaaaaaa");
    TEST_ASSERT_EQUAL(RCP_SYNC_ERR_IMAGE_REJECTED, hosted_push());
    TEST_ASSERT_NOT_NULL(strstr(detail, "upload c6_installer.bin first"));
    TEST_ASSERT_EQUAL(0, hs.ota_count);
    TEST_ASSERT_FALSE(rcp_sync_last_push_was_installer());
}

TEST_CASE("On the ESP-Hosted OTA failing: RCP_SYNC_ERR_HOSTED_OTA with its step, no wait", "[hosted]")
{
    hosted_setup(true);
    make_image(RCP_SYNC_INSTALLER_PROJECT, BASE_VERSION);
    hs.ota_err = ESP_FAIL;
    TEST_ASSERT_EQUAL(RCP_SYNC_ERR_HOSTED_OTA, hosted_push());
    TEST_ASSERT_EQUAL_STRING("OTAWrite refused (-1)", detail);
    TEST_ASSERT_EQUAL(0, hs.attach_count);
}

TEST_CASE("On no HELLO after the installer: ESP_ERR_TIMEOUT, the C6 did not come back", "[hosted]")
{
    hosted_setup(true);
    make_image(RCP_SYNC_INSTALLER_PROJECT, BASE_VERSION);
    hs.comes_back = false;
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, hosted_push());
    TEST_ASSERT_EQUAL_STRING("the C6 did not come back after the installer", detail);
    TEST_ASSERT_EQUAL(1, c6.enter_count);
    TEST_ASSERT_FALSE(rcp_sync_last_push_was_installer());
}

TEST_CASE("On push of a c6_installer to our updater: image rejected before BEGIN, ESP-Hosted never looked for", "[hosted]")
{
    hosted_setup(false);
    make_image(RCP_SYNC_INSTALLER_PROJECT, BASE_VERSION);
    TEST_ASSERT_EQUAL(RCP_SYNC_ERR_IMAGE_REJECTED, hosted_push());
    TEST_ASSERT_EQUAL(0, c6.begin_count);
    TEST_ASSERT_EQUAL(0, hs.detect_count);
}

TEST_CASE("On push with the jumper present and the updater silent: ESP-Hosted never looked for, updater not responding", "[hosted]")
{
    hosted_setup(true);
    hs.jumper = true;
    make_image(RCP_SYNC_INSTALLER_PROJECT, BASE_VERSION);
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, hosted_push());
    TEST_ASSERT_EQUAL_STRING("updater not responding", detail);
    TEST_ASSERT_EQUAL(0, hs.detect_count);
}

TEST_CASE("On push with the updater silent and no ESP-Hosted: updater not responding as before", "[hosted]")
{
    hosted_setup(true);
    hs.present = false;
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, hosted_push());
    TEST_ASSERT_EQUAL_STRING("updater not responding", detail);
    TEST_ASSERT_EQUAL(1, hs.detect_count);
    TEST_ASSERT_EQUAL(0, hs.ota_count);
}

TEST_CASE("On a normal push after an installer push: last_push_was_installer is false again", "[hosted]")
{
    hosted_setup(true);
    make_image(RCP_SYNC_INSTALLER_PROJECT, BASE_VERSION);
    TEST_ASSERT_EQUAL(ESP_OK, hosted_push());
    hosted_setup(false);
    TEST_ASSERT_EQUAL(ESP_OK, hosted_push());
    TEST_ASSERT_FALSE(rcp_sync_last_push_was_installer());
}

void app_main(void)
{
    UNITY_BEGIN();
    unity_run_all_tests();
    int failures = (int)UNITY_END();
    exit(failures == 0 ? 0 : 1);
}
