#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "unity.h"
#include "unity_test_runner.h"
#include "ota_stream.h"

#define WEB_PART_SIZE 4096
#define APP_PART_SIZE 8192
#define WEB_LEN 1000
#define APP_LEN 2500

/* ---- SHA-256 (FIPS 180-4), test-local so the host build needs no crypto component ---- */

static uint32_t rotr(uint32_t x, int n)
{
    return (x >> n) | (x << (32 - n));
}

static void sha256(const uint8_t *data, size_t len, uint8_t out[32])
{
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    size_t total = ((len + 9 + 63) / 64) * 64;
    uint8_t *msg = calloc(1, total);
    memcpy(msg, data, len);
    msg[len] = 0x80;
    uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++) {
        msg[total - 1 - i] = (uint8_t)(bits >> (8 * i));
    }
    for (size_t off = 0; off < total; off += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; i++) {
            const uint8_t *p = msg + off + 4 * i;
            w[i] = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
        }
        for (int i = 16; i < 64; i++) {
            uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; i++) {
            uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    free(msg);
    for (int i = 0; i < 8; i++) {
        out[4 * i] = (uint8_t)(h[i] >> 24);
        out[4 * i + 1] = (uint8_t)(h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(h[i] >> 8);
        out[4 * i + 3] = (uint8_t)h[i];
    }
}

/* ---- fake flash backend ----
 * Two web partitions and the ota partition are modelled; the backend only ever gets handed web_M and ota_M, the
 * "running" copies are canaries that must stay byte-identical. */

typedef struct {
    uint8_t web_running[WEB_PART_SIZE];
    uint8_t web_m[WEB_PART_SIZE];
    uint8_t app_running[16];
    uint8_t app_m[APP_PART_SIZE];
    bool web_erased;
    bool app_begun;
    bool app_open;
    int app_begin_calls, app_end_calls, app_abort_calls, commit_calls, web_verify_calls;
    int web_writes_before_erase;
    int app_writes_before_verify;
    bool web_verified;
    size_t app_got;
    esp_err_t fail_app_end;
    esp_err_t fail_web_write;
} fake_t;

static fake_t fk;

static esp_err_t f_web_begin(void *ctx, uint32_t web_len)
{
    if (web_len > WEB_PART_SIZE) {
        return ESP_ERR_INVALID_SIZE;
    }
    memset(fk.web_m, 0xFF, sizeof(fk.web_m));
    fk.web_erased = true;
    return ESP_OK;
}

static esp_err_t f_web_write(void *ctx, uint32_t off, const void *data, size_t len)
{
    if (!fk.web_erased) {
        fk.web_writes_before_erase++;
    }
    if (fk.fail_web_write != ESP_OK) {
        return fk.fail_web_write;
    }
    TEST_ASSERT_TRUE(off + len <= WEB_PART_SIZE);
    memcpy(fk.web_m + off, data, len);
    return ESP_OK;
}

static esp_err_t f_web_verify(void *ctx, uint32_t web_len, const uint8_t expect[32])
{
    uint8_t got[32];
    fk.web_verify_calls++;
    sha256(fk.web_m, web_len, got);
    if (memcmp(got, expect, 32) != 0) {
        return ESP_ERR_INVALID_CRC;
    }
    fk.web_verified = true;
    return ESP_OK;
}

static esp_err_t f_app_begin(void *ctx)
{
    fk.app_begin_calls++;
    if (!fk.web_verified) {
        fk.app_writes_before_verify++;
    }
    fk.app_open = true;
    return ESP_OK;
}

static esp_err_t f_app_write(void *ctx, const void *data, size_t len)
{
    TEST_ASSERT_TRUE(fk.app_open);
    TEST_ASSERT_TRUE(fk.app_got + len <= APP_PART_SIZE);
    memcpy(fk.app_m + fk.app_got, data, len);
    fk.app_got += len;
    return ESP_OK;
}

static esp_err_t f_app_end(void *ctx)
{
    fk.app_end_calls++;
    fk.app_open = false; /* esp_ota_end releases the handle even when validation fails */
    return fk.fail_app_end;
}

static void f_app_abort(void *ctx)
{
    fk.app_abort_calls++;
    fk.app_open = false;
}

static esp_err_t f_commit(void *ctx)
{
    fk.commit_calls++;
    return ESP_OK;
}

static const ota_stream_ops_t OPS = {f_web_begin, f_web_write, f_web_verify, f_app_begin,
                                     f_app_write, f_app_end, f_app_abort, f_commit};

/* ---- bundle fixture ---- */

static uint8_t web_img[WEB_LEN];
static uint8_t app_img[APP_LEN];
static uint8_t bundle[OTA_BUNDLE_HEADER_LEN + WEB_LEN + APP_LEN];
static size_t bundle_len;

static void build_bundle(bool corrupt_sha)
{
    for (size_t i = 0; i < sizeof(web_img); i++) {
        web_img[i] = (uint8_t)(i * 7 + 1);
    }
    for (size_t i = 0; i < sizeof(app_img); i++) {
        app_img[i] = (uint8_t)(i * 13 + 5);
    }
    memset(bundle, 0, sizeof(bundle));
    memcpy(bundle, "OTBB", 4);
    bundle[4] = 1;
    bundle[8] = WEB_LEN & 0xFF;
    bundle[9] = WEB_LEN >> 8;
    sha256(web_img, WEB_LEN, bundle + 12);
    if (corrupt_sha) {
        bundle[12] ^= 1;
    }
    memcpy(bundle + OTA_BUNDLE_HEADER_LEN, web_img, WEB_LEN);
    memcpy(bundle + OTA_BUNDLE_HEADER_LEN + WEB_LEN, app_img, APP_LEN);
    bundle_len = sizeof(bundle);
}

static void setup(bool corrupt_sha)
{
    memset(&fk, 0, sizeof(fk));
    memset(fk.web_running, 0xC3, sizeof(fk.web_running));
    memset(fk.app_running, 0x3C, sizeof(fk.app_running));
    build_bundle(corrupt_sha);
}

static esp_err_t run_chunked(ota_stream_t *s, size_t total, size_t chunk)
{
    esp_err_t err = ESP_OK;
    for (size_t off = 0; off < total && err == ESP_OK; off += chunk) {
        size_t n = total - off < chunk ? total - off : chunk;
        err = ota_stream_write(s, bundle + off, n);
    }
    return err;
}

static void assert_running_pair_untouched(void)
{
    for (size_t i = 0; i < sizeof(fk.web_running); i++) {
        TEST_ASSERT_EQUAL_HEX8(0xC3, fk.web_running[i]);
    }
    for (size_t i = 0; i < sizeof(fk.app_running); i++) {
        TEST_ASSERT_EQUAL_HEX8(0x3C, fk.app_running[i]);
    }
}

/* ---- tests ---- */

TEST_CASE("On valid bundle: writes web_M and ota_M, commits once", "[ota_stream]")
{
    setup(false);
    ota_stream_t s;
    ota_stream_init(&s, &OPS, NULL);
    TEST_ASSERT_EQUAL(ESP_OK, ota_stream_write(&s, bundle, bundle_len));
    TEST_ASSERT_EQUAL(ESP_OK, ota_stream_finish(&s));
    TEST_ASSERT_EQUAL_MEMORY(web_img, fk.web_m, WEB_LEN);
    TEST_ASSERT_EQUAL_MEMORY(app_img, fk.app_m, APP_LEN);
    TEST_ASSERT_EQUAL_UINT(APP_LEN, fk.app_got);
    TEST_ASSERT_EQUAL(1, fk.commit_calls);
    TEST_ASSERT_EQUAL(0, fk.app_abort_calls);
    assert_running_pair_untouched();
}

TEST_CASE("On web_M write: erases the partition first and compares the SHA-256 before the app image", "[ota_stream]")
{
    setup(false);
    ota_stream_t s;
    ota_stream_init(&s, &OPS, NULL);
    TEST_ASSERT_EQUAL(ESP_OK, ota_stream_write(&s, bundle, bundle_len));
    TEST_ASSERT_EQUAL(ESP_OK, ota_stream_finish(&s));
    TEST_ASSERT_EQUAL(0, fk.web_writes_before_erase);
    TEST_ASSERT_EQUAL(1, fk.web_verify_calls);
    TEST_ASSERT_EQUAL(0, fk.app_writes_before_verify);
    /* bytes behind the web image stay erased */
    TEST_ASSERT_EQUAL_HEX8(0xFF, fk.web_m[WEB_LEN]);
    TEST_ASSERT_EQUAL_HEX8(0xFF, fk.web_m[WEB_PART_SIZE - 1]);
}

TEST_CASE("On any chunk size (1, 7, 43, 44, 45, 1000, 1044, 4096 bytes): result equals a single write", "[ota_stream]")
{
    static const size_t chunks[] = {1, 7, 43, 44, 45, 1000, 1044, 4096};
    for (size_t c = 0; c < sizeof(chunks) / sizeof(chunks[0]); c++) {
        setup(false);
        ota_stream_t s;
        ota_stream_init(&s, &OPS, NULL);
        TEST_ASSERT_EQUAL(ESP_OK, run_chunked(&s, bundle_len, chunks[c]));
        TEST_ASSERT_EQUAL(ESP_OK, ota_stream_finish(&s));
        TEST_ASSERT_EQUAL_MEMORY(web_img, fk.web_m, WEB_LEN);
        TEST_ASSERT_EQUAL_MEMORY(app_img, fk.app_m, APP_LEN);
        TEST_ASSERT_EQUAL(1, fk.commit_calls);
        TEST_ASSERT_EQUAL(1, fk.app_begin_calls);
    }
}

TEST_CASE("On web SHA-256 mismatch: aborts before ota_M is opened, never commits", "[ota_stream]")
{
    setup(true);
    ota_stream_t s;
    ota_stream_init(&s, &OPS, NULL);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_CRC, ota_stream_write(&s, bundle, bundle_len));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, ota_stream_write(&s, bundle, 1));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, ota_stream_finish(&s));
    ota_stream_abort(&s);
    TEST_ASSERT_EQUAL(0, fk.app_begin_calls);
    TEST_ASSERT_EQUAL(0, fk.commit_calls);
    assert_running_pair_untouched();
}

TEST_CASE("On invalid app image: finish fails and never commits", "[ota_stream]")
{
    setup(false);
    fk.fail_app_end = ESP_FAIL;
    ota_stream_t s;
    ota_stream_init(&s, &OPS, NULL);
    TEST_ASSERT_EQUAL(ESP_OK, ota_stream_write(&s, bundle, bundle_len));
    TEST_ASSERT_EQUAL(ESP_FAIL, ota_stream_finish(&s));
    ota_stream_abort(&s);
    TEST_ASSERT_EQUAL(0, fk.commit_calls);
    TEST_ASSERT_FALSE(fk.app_open);
    assert_running_pair_untouched();
}

TEST_CASE("On bundle truncated at every offset: finish fails, never commits, releases the ota session", "[ota_stream]")
{
    setup(false);
    for (size_t cut = 0; cut < bundle_len; cut++) {
        setup(false);
        ota_stream_t s;
        ota_stream_init(&s, &OPS, NULL);
        TEST_ASSERT_EQUAL(ESP_OK, ota_stream_write(&s, bundle, cut));
        if (cut > OTA_BUNDLE_HEADER_LEN + WEB_LEN) {
            /* app bytes were received: a short app image is only caught by the image validation */
            fk.fail_app_end = ESP_FAIL;
        }
        TEST_ASSERT_NOT_EQUAL(ESP_OK, ota_stream_finish(&s));
        ota_stream_abort(&s);
        TEST_ASSERT_EQUAL(0, fk.commit_calls);
        TEST_ASSERT_FALSE(fk.app_open);
        assert_running_pair_untouched();
    }
}

TEST_CASE("On abort mid-app: releases the ota session once, never commits, abort is repeatable", "[ota_stream]")
{
    setup(false);
    ota_stream_t s;
    ota_stream_init(&s, &OPS, NULL);
    TEST_ASSERT_EQUAL(ESP_OK, ota_stream_write(&s, bundle, OTA_BUNDLE_HEADER_LEN + WEB_LEN + 100));
    TEST_ASSERT_TRUE(fk.app_open);
    ota_stream_abort(&s);
    ota_stream_abort(&s);
    TEST_ASSERT_EQUAL(1, fk.app_abort_calls);
    TEST_ASSERT_FALSE(fk.app_open);
    TEST_ASSERT_EQUAL(0, fk.commit_calls);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, ota_stream_write(&s, bundle, 1));
}

TEST_CASE("On wrong magic: fails at the header without touching flash", "[ota_stream]")
{
    setup(false);
    bundle[0] = 'X';
    ota_stream_t s;
    ota_stream_init(&s, &OPS, NULL);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_VERSION, ota_stream_write(&s, bundle, bundle_len));
    TEST_ASSERT_FALSE(fk.web_erased);
    TEST_ASSERT_EQUAL(0, fk.commit_calls);
}

TEST_CASE("On web_len zero or larger than the partition: fails with ESP_ERR_INVALID_SIZE before writing", "[ota_stream]")
{
    setup(false);
    bundle[8] = bundle[9] = 0;
    ota_stream_t s;
    ota_stream_init(&s, &OPS, NULL);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, ota_stream_write(&s, bundle, bundle_len));
    TEST_ASSERT_FALSE(fk.web_erased);
    setup(false);
    bundle[10] = 1; /* web_len 0x10000 + 1000 > WEB_PART_SIZE */
    ota_stream_init(&s, &OPS, NULL);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, ota_stream_write(&s, bundle, bundle_len));
    TEST_ASSERT_FALSE(fk.web_erased);
}

TEST_CASE("On web flash write error: returns it, never commits, later writes are refused", "[ota_stream]")
{
    setup(false);
    fk.fail_web_write = ESP_FAIL;
    ota_stream_t s;
    ota_stream_init(&s, &OPS, NULL);
    TEST_ASSERT_EQUAL(ESP_FAIL, ota_stream_write(&s, bundle, bundle_len));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, ota_stream_write(&s, bundle, 1));
    TEST_ASSERT_EQUAL(0, fk.commit_calls);
}

TEST_CASE("On finish after a finished stream: returns ESP_ERR_INVALID_STATE, commits once", "[ota_stream]")
{
    setup(false);
    ota_stream_t s;
    ota_stream_init(&s, &OPS, NULL);
    TEST_ASSERT_EQUAL(ESP_OK, ota_stream_write(&s, bundle, bundle_len));
    TEST_ASSERT_EQUAL(ESP_OK, ota_stream_finish(&s));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, ota_stream_finish(&s));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, ota_stream_write(&s, bundle, 1));
    TEST_ASSERT_EQUAL(1, fk.commit_calls);
}

void app_main(void)
{
    UNITY_BEGIN();
    unity_run_all_tests();
    int failures = (int)UNITY_END();
    exit(failures == 0 ? 0 : 1);
}

/* ---- ota_stream_start_check ---- */

TEST_CASE("On a confirmed running image and nothing committed: an update may start", "[ota_stream]")
{
    TEST_ASSERT_EQUAL(ESP_OK, ota_stream_start_check(true, false));
}

TEST_CASE("On a committed update waiting for its reboot: ESP_ERR_INVALID_STATE", "[ota_stream]")
{
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, ota_stream_start_check(false, false));
}

TEST_CASE("On a running image pending verification: ESP_BR_OTA_ERR_NOT_CONFIRMED, before anything is erased", "[ota_stream]")
{
    TEST_ASSERT_EQUAL(ESP_BR_OTA_ERR_NOT_CONFIRMED, ota_stream_start_check(true, true));
}

TEST_CASE("On a committed update and a pending running image: ESP_ERR_INVALID_STATE (the commit wins)", "[ota_stream]")
{
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, ota_stream_start_check(false, true));
}
