/*
 * Paired OTA core (patch 5): streams a bundle (web image + P4 app image) into the inactive web_M and ota_M.
 * The chunk state machine is ota_stream.c; this file is its flash backend and the single-update guard.
 */

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "esp_br_http_ota.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ota_stream.h"
#include "psa/crypto.h"

#define TAG "BR_OTA"
#define VERIFY_CHUNK 512

struct esp_br_ota {
    ota_stream_t stream;
    const esp_partition_t *web;     /* web_M: never the running web slot */
    const esp_partition_t *ota;     /* ota_M: never the running app slot */
    esp_ota_handle_t ota_handle;
    uint32_t web_len;
};

static struct esp_br_ota s_ota;
static atomic_bool s_busy;
#ifdef OTBR_TEST_BUILD
static uint32_t s_commit_delay_ms;

void esp_br_ota_test_set_commit_delay_ms(uint32_t ms)
{
    s_commit_delay_ms = ms;
}
#endif

static esp_err_t web_begin(void *ctx, uint32_t web_len)
{
    struct esp_br_ota *ota = ctx;
    ESP_RETURN_ON_FALSE(web_len <= ota->web->size, ESP_ERR_INVALID_SIZE, TAG, "web image does not fit %s", ota->web->label);
    ESP_LOGI(TAG, "erasing %s, writing %u web bytes", ota->web->label, (unsigned)web_len);
    ota->web_len = web_len;
    return esp_partition_erase_range(ota->web, 0, ota->web->size);
}

static esp_err_t web_write(void *ctx, uint32_t offset, const void *data, size_t len)
{
    struct esp_br_ota *ota = ctx;
    ESP_RETURN_ON_FALSE(offset + len <= ota->web_len, ESP_ERR_INVALID_SIZE, TAG, "web write beyond web_len");
    return esp_partition_write(ota->web, offset, data, len);
}

/* Read the web image back from flash and compare its SHA-256 (verifies what was stored, not what was received). */
static esp_err_t web_verify(void *ctx, uint32_t web_len, const uint8_t expected[32])
{
    struct esp_br_ota *ota = ctx;
    uint8_t chunk[VERIFY_CHUNK];
    uint8_t digest[32];
    size_t digest_len = 0;
    psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;

    ESP_RETURN_ON_FALSE(psa_crypto_init() == PSA_SUCCESS, ESP_FAIL, TAG, "psa init");
    ESP_RETURN_ON_FALSE(psa_hash_setup(&op, PSA_ALG_SHA_256) == PSA_SUCCESS, ESP_FAIL, TAG, "sha setup");
    for (uint32_t off = 0; off < web_len; off += sizeof(chunk)) {
        size_t n = web_len - off < sizeof(chunk) ? web_len - off : sizeof(chunk);
        esp_err_t err = esp_partition_read(ota->web, off, chunk, n);
        if (err != ESP_OK || psa_hash_update(&op, chunk, n) != PSA_SUCCESS) {
            psa_hash_abort(&op);
            return err != ESP_OK ? err : ESP_FAIL;
        }
    }
    ESP_RETURN_ON_FALSE(psa_hash_finish(&op, digest, sizeof(digest), &digest_len) == PSA_SUCCESS, ESP_FAIL, TAG, "sha finish");
    ESP_RETURN_ON_FALSE(memcmp(digest, expected, sizeof(digest)) == 0, ESP_ERR_INVALID_CRC, TAG, "web SHA-256 mismatch");
    return ESP_OK;
}

static esp_err_t app_begin(void *ctx)
{
    struct esp_br_ota *ota = ctx;
    ESP_LOGI(TAG, "writing the application image to %s", ota->ota->label);
    return esp_ota_begin(ota->ota, OTA_WITH_SEQUENTIAL_WRITES, &ota->ota_handle);
}

static esp_err_t app_write(void *ctx, const void *data, size_t len)
{
    struct esp_br_ota *ota = ctx;
    return esp_ota_write(ota->ota_handle, data, len);
}

/* esp_ota_end validates the image and releases the handle on every path. */
static esp_err_t app_end(void *ctx)
{
    struct esp_br_ota *ota = ctx;
    esp_err_t err = esp_ota_end(ota->ota_handle);
    ota->ota_handle = 0;
    return err;
}

static void app_abort(void *ctx)
{
    struct esp_br_ota *ota = ctx;
    esp_ota_abort(ota->ota_handle);
    ota->ota_handle = 0;
}

/* The only commit point of an update. */
static esp_err_t commit(void *ctx)
{
    struct esp_br_ota *ota = ctx;
#ifdef OTBR_TEST_BUILD
    if (s_commit_delay_ms) {
        ESP_LOGW(TAG, "test build: committing in %u ms", (unsigned)s_commit_delay_ms);
        vTaskDelay(pdMS_TO_TICKS(s_commit_delay_ms));
    }
#endif
    ESP_RETURN_ON_ERROR(esp_ota_set_boot_partition(ota->ota), TAG, "Failed to set boot partition");
    ESP_LOGI(TAG, "boot partition set to %s (web image in %s)", ota->ota->label, ota->web->label);
    return ESP_OK;
}

static const ota_stream_ops_t s_ops = {
    .web_begin = web_begin,
    .web_write = web_write,
    .web_verify = web_verify,
    .app_begin = app_begin,
    .app_write = app_write,
    .app_end = app_end,
    .app_abort = app_abort,
    .commit = commit,
};

/* Pick the inactive pair. Never returns a partition of the running slot, and refuses while a committed update waits
 * for its reboot (the boot partition then differs from the running one and must not be rewritten) and while the
 * running image is pending verification (its rollback target's web slot is the inactive one). */
static esp_err_t select_inactive_pair(const esp_partition_t **web, const esp_partition_t **ota)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    ESP_RETURN_ON_FALSE(running != NULL, ESP_ERR_INVALID_STATE, TAG, "no running partition");
    esp_ota_img_states_t running_state = ESP_OTA_IMG_UNDEFINED;
    esp_ota_get_state_partition(running, &running_state); /* fails without rollback data: not pending */
    ESP_RETURN_ON_ERROR(ota_stream_start_check(esp_ota_get_boot_partition() == running, running_state == ESP_OTA_IMG_PENDING_VERIFY),
                        TAG, "update already committed, or the running image is not confirmed yet");
    const esp_partition_t *next = esp_ota_get_next_update_partition(running);
    ESP_RETURN_ON_FALSE(next != NULL && next != running, ESP_ERR_NOT_FOUND, TAG, "no inactive ota partition");
    int running_slot = running->subtype - ESP_PARTITION_SUBTYPE_APP_OTA_MIN;
    int next_slot = next->subtype - ESP_PARTITION_SUBTYPE_APP_OTA_MIN;
    ESP_RETURN_ON_FALSE(next_slot != running_slot && next_slot >= 0 && next_slot <= 1, ESP_ERR_NOT_SUPPORTED, TAG, "unexpected ota layout");

    char label[8];
    snprintf(label, sizeof(label), "web_%d", next_slot);
    const esp_partition_t *web_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, label);
    ESP_RETURN_ON_FALSE(web_part != NULL, ESP_ERR_NOT_FOUND, TAG, "partition %s not found", label);
    *web = web_part;
    *ota = next;
    return ESP_OK;
}

esp_err_t esp_br_ota_begin(esp_br_ota_handle_t *out)
{
    ESP_RETURN_ON_FALSE(out != NULL, ESP_ERR_INVALID_ARG, TAG, "out is NULL");
    bool expected = false;
    if (!atomic_compare_exchange_strong(&s_busy, &expected, true)) {
        ESP_LOGE(TAG, "an update is already in progress");
        return ESP_ERR_INVALID_STATE;
    }
    memset(&s_ota, 0, sizeof(s_ota));
    esp_err_t err = select_inactive_pair(&s_ota.web, &s_ota.ota);
    if (err != ESP_OK) {
        atomic_store(&s_busy, false);
        return err;
    }
    ota_stream_init(&s_ota.stream, &s_ops, &s_ota);
    *out = &s_ota;
    return ESP_OK;
}

esp_err_t esp_br_ota_write(esp_br_ota_handle_t h, const void *data, size_t len)
{
    ESP_RETURN_ON_FALSE(h == &s_ota && atomic_load(&s_busy), ESP_ERR_INVALID_STATE, TAG, "no update in progress");
    return ota_stream_write(&h->stream, data, len);
}

esp_err_t esp_br_ota_end(esp_br_ota_handle_t h)
{
    ESP_RETURN_ON_FALSE(h == &s_ota && atomic_load(&s_busy), ESP_ERR_INVALID_STATE, TAG, "no update in progress");
    esp_err_t err = ota_stream_finish(&h->stream);
    ota_stream_abort(&h->stream); /* no-op after a clean finish; releases a still open session after a failure */
    atomic_store(&s_busy, false);
    return err;
}

void esp_br_ota_abort(esp_br_ota_handle_t h)
{
    if (h != &s_ota || !atomic_load(&s_busy)) {
        return;
    }
    ota_stream_abort(&h->stream);
    atomic_store(&s_busy, false);
}
