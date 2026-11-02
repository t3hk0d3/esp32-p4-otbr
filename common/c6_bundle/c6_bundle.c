#include <stdlib.h>
#include <string.h>
#include "psa/crypto.h"
#include "c6_board.h"
#include "c6_bundle.h"

#define ENTRY_LEN    (4 + 4 + C6_BUNDLE_SHA256_LEN)
#define ENTRIES_AT   (C6_BUNDLE_MAGIC_LEN + 4 + 4)
#define HASH_AT      (ENTRIES_AT + C6_BUNDLE_ENTRIES * ENTRY_LEN)

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

uint32_t c6_bundle_target_offset(c6_bundle_kind_t kind)
{
    switch (kind) {
    case C6_BUNDLE_BOOTLOADER:
        return C6_FLASH_BOOTLOADER_OFFSET;
    case C6_BUNDLE_PTABLE:
        return C6_FLASH_PTABLE_OFFSET;
    case C6_BUNDLE_UPDATER:
        return C6_FLASH_TEST_OFFSET;
    case C6_BUNDLE_RCP:
        return C6_FLASH_OTA0_OFFSET;
    default:
        return 0;
    }
}

uint32_t c6_bundle_target_room(c6_bundle_kind_t kind)
{
    switch (kind) {
    case C6_BUNDLE_BOOTLOADER:
        return C6_FLASH_PTABLE_OFFSET - C6_FLASH_BOOTLOADER_OFFSET;
    case C6_BUNDLE_PTABLE:
        return C6_FLASH_NVS_OFFSET - C6_FLASH_PTABLE_OFFSET;
    case C6_BUNDLE_UPDATER:
        return C6_FLASH_TEST_SIZE;
    case C6_BUNDLE_RCP:
        // The RCP slot is larger, but the migrator must not write into stock ota_1, where the transfer usually is.
        return C6_BUNDLE_TARGET_END - C6_FLASH_OTA0_OFFSET;
    default:
        return 0;
    }
}

uint32_t c6_bundle_header_offset(uint32_t image_len)
{
    return (image_len + C6_BUNDLE_ALIGN - 1) & ~(uint32_t)(C6_BUNDLE_ALIGN - 1);
}

uint32_t c6_bundle_data_len(const c6_bundle_t *bundle)
{
    uint32_t total = 0;

    for (int i = 0; i < C6_BUNDLE_ENTRIES; i++) {
        total += bundle->entries[i].length;
    }
    return total;
}

static bool entries_valid(const c6_bundle_t *b)
{
    for (int i = 0; i < C6_BUNDLE_ENTRIES; i++) {
        const c6_bundle_entry_t *e = &b->entries[i];
        if (e->kind != (uint32_t)(i + 1) || e->length == 0 || e->length > c6_bundle_target_room((c6_bundle_kind_t)e->kind)) {
            return false;
        }
    }
    return true;
}

esp_err_t c6_bundle_header_encode(const c6_bundle_t *bundle, uint8_t *out, size_t out_cap)
{
    if (!bundle || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    if (out_cap < C6_BUNDLE_HEADER_LEN || !entries_valid(bundle)) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(out, C6_BUNDLE_MAGIC, C6_BUNDLE_MAGIC_LEN);
    put_u32(out + C6_BUNDLE_MAGIC_LEN, C6_BUNDLE_VERSION);
    put_u32(out + C6_BUNDLE_MAGIC_LEN + 4, C6_BUNDLE_ENTRIES);
    for (int i = 0; i < C6_BUNDLE_ENTRIES; i++) {
        uint8_t *p = out + ENTRIES_AT + i * ENTRY_LEN;
        put_u32(p, bundle->entries[i].kind);
        put_u32(p + 4, bundle->entries[i].length);
        memcpy(p + 8, bundle->entries[i].sha256, C6_BUNDLE_SHA256_LEN);
    }
    return c6_bundle_sha256(out, HASH_AT, out + HASH_AT);
}

esp_err_t c6_bundle_header_decode(const uint8_t *buf, size_t len, c6_bundle_t *out)
{
    if (!buf || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len < C6_BUNDLE_HEADER_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (memcmp(buf, C6_BUNDLE_MAGIC, C6_BUNDLE_MAGIC_LEN) != 0 || get_u32(buf + C6_BUNDLE_MAGIC_LEN) != C6_BUNDLE_VERSION ||
            get_u32(buf + C6_BUNDLE_MAGIC_LEN + 4) != C6_BUNDLE_ENTRIES) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    uint8_t digest[C6_BUNDLE_SHA256_LEN];
    esp_err_t err = c6_bundle_sha256(buf, HASH_AT, digest);
    if (err != ESP_OK) {
        return err;
    }
    if (memcmp(digest, buf + HASH_AT, sizeof(digest)) != 0) {
        return ESP_ERR_INVALID_CRC;
    }
    c6_bundle_t b;
    for (int i = 0; i < C6_BUNDLE_ENTRIES; i++) {
        const uint8_t *p = buf + ENTRIES_AT + i * ENTRY_LEN;
        b.entries[i].kind = get_u32(p);
        b.entries[i].length = get_u32(p + 4);
        memcpy(b.entries[i].sha256, p + 8, C6_BUNDLE_SHA256_LEN);
    }
    if (!entries_valid(&b)) {
        return ESP_ERR_INVALID_SIZE;
    }
    *out = b;
    return ESP_OK;
}

esp_err_t c6_bundle_sha256_begin(c6_bundle_sha256_t *ctx)
{
    if (!ctx) {
        return ESP_ERR_INVALID_ARG;
    }
    ctx->op = NULL;
    if (psa_crypto_init() != PSA_SUCCESS) {
        return ESP_FAIL;
    }
    psa_hash_operation_t *op = calloc(1, sizeof(*op));
    if (!op) {
        return ESP_ERR_NO_MEM;
    }
    *op = psa_hash_operation_init();
    if (psa_hash_setup(op, PSA_ALG_SHA_256) != PSA_SUCCESS) {
        free(op);
        return ESP_FAIL;
    }
    ctx->op = op;
    return ESP_OK;
}

esp_err_t c6_bundle_sha256_update(c6_bundle_sha256_t *ctx, const void *data, size_t len)
{
    if (!ctx || !ctx->op || (!data && len)) {
        return ESP_ERR_INVALID_ARG;
    }
    return psa_hash_update(ctx->op, data, len) == PSA_SUCCESS ? ESP_OK : ESP_FAIL;
}

void c6_bundle_sha256_abort(c6_bundle_sha256_t *ctx)
{
    if (ctx && ctx->op) {
        psa_hash_abort(ctx->op);
        free(ctx->op);
        ctx->op = NULL;
    }
}

esp_err_t c6_bundle_sha256_finish(c6_bundle_sha256_t *ctx, uint8_t out[C6_BUNDLE_SHA256_LEN])
{
    if (!ctx || !ctx->op || !out) {
        c6_bundle_sha256_abort(ctx);
        return ESP_ERR_INVALID_ARG;
    }
    size_t n = 0;
    const psa_status_t st = psa_hash_finish(ctx->op, out, C6_BUNDLE_SHA256_LEN, &n);
    c6_bundle_sha256_abort(ctx);
    return st == PSA_SUCCESS && n == C6_BUNDLE_SHA256_LEN ? ESP_OK : ESP_FAIL;
}

esp_err_t c6_bundle_sha256(const void *data, size_t len, uint8_t out[C6_BUNDLE_SHA256_LEN])
{
    c6_bundle_sha256_t ctx;
    esp_err_t err = c6_bundle_sha256_begin(&ctx);

    if (err == ESP_OK) {
        err = c6_bundle_sha256_update(&ctx, data, len);
    }
    if (err != ESP_OK) {
        c6_bundle_sha256_abort(&ctx);
        return err;
    }
    return c6_bundle_sha256_finish(&ctx, out);
}
