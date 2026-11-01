#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "link.h"
#include "rcp_sync_proto.h"
#include "recovery_policy.h"

static const char *TAG = "rcp_sync";

#define PROBE_ATTEMPTS      3 /* updater entry tries for the probe */
#define PUSH_ATTEMPTS       3 /* whole-transfer tries for a re-openable source */
#define EXCHANGE_TRIES      3
#define HELLO_TIMEOUT_MS    500
#define BEGIN_TIMEOUT_MS    30000
#define DATA_TIMEOUT_MS     1000
#define END_TIMEOUT_MS      30000 /* updater_core.h contract: END verifies the image, like BEGIN erases */
#define DATA_OFFSET_LEN     4
#define CHUNK_MAX           (RCP_SYNC_PROTO_MAX_PAYLOAD - DATA_OFFSET_LEN)
#define REQ_BUF_LEN         (RCP_SYNC_PROTO_HEADER_LEN + RCP_SYNC_PROTO_MAX_PAYLOAD + RCP_SYNC_PROTO_CRC_LEN)
/* Largest reply is HELLO; round up to the SPI word size. */
#define REPLY_BUF_LEN       ((RCP_SYNC_PROTO_HEADER_LEN + RCP_SYNC_PROTO_HELLO_REPLY_LEN + RCP_SYNC_PROTO_CRC_LEN + 3) & ~3u)
#define ERR_PAYLOAD_LEN     6

typedef struct {
    const rcp_sync_link_t *link;
    uint8_t seq;
    uint16_t err_code; /* code of the last ERR reply */
    uint8_t req[REQ_BUF_LEN];
    uint8_t reply[REPLY_BUF_LEN];
} sync_ctx_t;

/* Written only by rcp_sync_probe (br_core calls it from the boot path and the push path); read from any task through
 * rcp_sync_get_last_probe(), which copies under s_last_lock so a reader never sees a half-written result. */
static rcp_sync_probe_result_t s_last;
static portMUX_TYPE s_last_lock = portMUX_INITIALIZER_UNLOCKED;
/* An automatic ROM recovery ran in this boot (spec 5.10: at most once, whatever its outcome). Same writers as s_last. */
static bool s_auto_recovery_done;

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/**
 * Send one request with a fresh seq and decode its reply. NAK, timeout and undecodable or mismatching
 * replies are retried by resending the identical frame (same seq), which the updater answers idempotently.
 * An OK reply is returned in *out (payload points into ctx->reply); an ERR reply is ESP_FAIL.
 */
static esp_err_t exchange(sync_ctx_t *ctx, rcp_sync_msg_t type, const uint8_t *payload, uint16_t len,
                          uint32_t timeout_ms, rcp_sync_proto_frame_t *out)
{
    const rcp_sync_proto_frame_t req = {.type = type, .seq = ++ctx->seq, .len = len, .payload = payload};
    size_t req_len = 0;
    esp_err_t err = rcp_sync_proto_encode(&req, ctx->req, sizeof(ctx->req), &req_len);
    if (err != ESP_OK) {
        return err;
    }

    err = ESP_ERR_TIMEOUT;
    for (int i = 0; i < EXCHANGE_TRIES; i++) {
        size_t reply_len = 0;
        err = ctx->link->transact(ctx->req, req_len, ctx->reply, sizeof(ctx->reply), &reply_len, timeout_ms);
        if (err != ESP_OK) {
            continue;
        }
        err = rcp_sync_proto_decode(ctx->reply, reply_len, out);
        if (err != ESP_OK) {
            continue;
        }
        if (out->type == RCP_SYNC_MSG_NAK) {
            err = ESP_ERR_INVALID_RESPONSE;
            continue;
        }
        if (out->seq != req.seq) {
            err = ESP_ERR_INVALID_RESPONSE;
            continue;
        }
        if (out->type == RCP_SYNC_MSG_ERR) {
            if (out->len == ERR_PAYLOAD_LEN) {
                ctx->err_code = (uint16_t)(out->payload[0] | (out->payload[1] << 8));
                ESP_LOGW(TAG, "request %d refused: code %u, expected offset %u", type,
                         (unsigned)(out->payload[0] | (out->payload[1] << 8)), (unsigned)get_u32(&out->payload[2]));
            }
            return ESP_FAIL;
        }
        return out->type == RCP_SYNC_MSG_OK ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
    }
    return err;
}

static esp_err_t hello(sync_ctx_t *ctx, rcp_sync_hello_reply_t *out)
{
    rcp_sync_proto_frame_t reply;
    esp_err_t err = exchange(ctx, RCP_SYNC_MSG_HELLO, NULL, 0, HELLO_TIMEOUT_MS, &reply);
    if (err != ESP_OK) {
        return err;
    }
    return rcp_sync_proto_get_hello_reply(reply.payload, reply.len, out);
}

static void set_text(char *dst, size_t cap, const char *msg)
{
    if (dst && cap > 0) {
        strlcpy(dst, msg, cap);
    }
}

/* Outcome of one whole-transfer attempt: whether another attempt may help. */
typedef enum {
    ATTEMPT_RETRY,
    ATTEMPT_FINAL,
} attempt_end_t;

/* BEGIN, DATA..., END for one attempt. The source is read strictly sequentially. */
static esp_err_t transfer(sync_ctx_t *ctx, const rcp_fw_source_t *source, uint8_t *chunk, char *detail, size_t cap,
                          attempt_end_t *end)
{
    const size_t size = source->total_size;
    rcp_sync_proto_frame_t reply;
    uint8_t size_payload[4];

    *end = ATTEMPT_RETRY;
    put_u32(size_payload, (uint32_t)size);
    ESP_LOGI(TAG, "BEGIN %u bytes", (unsigned)size);
    esp_err_t err = exchange(ctx, RCP_SYNC_MSG_BEGIN, size_payload, sizeof(size_payload), BEGIN_TIMEOUT_MS, &reply);
    if (err != ESP_OK) {
        set_text(detail, cap, "C6 refused or did not answer BEGIN");
        return err;
    }

    size_t offset = 0;
    while (offset < size) {
        size_t n = size - offset < CHUNK_MAX ? size - offset : CHUNK_MAX;
        put_u32(chunk, (uint32_t)offset);
        err = source->read(source->ctx, offset, chunk + DATA_OFFSET_LEN, n);
        if (err != ESP_OK) {
            set_text(detail, cap, "reading the firmware source failed");
            return err;
        }
        err = exchange(ctx, RCP_SYNC_MSG_DATA, chunk, (uint16_t)(DATA_OFFSET_LEN + n), DATA_TIMEOUT_MS, &reply);
        if (err == ESP_FAIL && offset == 0 && ctx->err_code == RCP_SYNC_ERR_FLASH) {
            // esp_ota_write on the C6 validates the image header with the first chunk: a refusal there is a bad image.
            set_text(detail, cap, "the C6 rejected the image header (a flash write error on the first chunk is reported the same way)");
            *end = ATTEMPT_FINAL;
            return RCP_SYNC_ERR_IMAGE_REJECTED;
        }
        if (err != ESP_OK) {
            set_text(detail, cap, "transfer to the C6 failed");
            return err;
        }
        uint32_t next = reply.len == 4 ? get_u32(reply.payload) : 0;
        if (next != offset + n) {
            set_text(detail, cap, "C6 acknowledged an unexpected offset");
            return ESP_ERR_INVALID_RESPONSE;
        }
        offset = next;
    }

    err = exchange(ctx, RCP_SYNC_MSG_END, NULL, 0, END_TIMEOUT_MS, &reply);
    if (err == ESP_FAIL && ctx->err_code == RCP_SYNC_ERR_VERIFY) {
        set_text(detail, cap, "the C6 rejected the image (verification failed)");
        *end = ATTEMPT_FINAL; // the same bytes would fail again
        return RCP_SYNC_ERR_IMAGE_REJECTED;
    } else if (err != ESP_OK) {
        set_text(detail, cap, "C6 did not confirm END");
    }
    return err;
}

/* Up to PROBE_ATTEMPTS updater entries with HELLO; fills the updater fields of out on success. */
static void probe_updater(sync_ctx_t *ctx, rcp_sync_probe_result_t *out)
{
    strlcpy(out->last_error, "updater not responding", sizeof(out->last_error));
    for (int attempt = 1; attempt <= PROBE_ATTEMPTS && !out->updater_reachable; attempt++) {
        rcp_sync_hello_reply_t h;
        if (ctx->link->enter_updater() != ESP_OK || hello(ctx, &h) != ESP_OK) {
            ESP_LOGW(TAG, "attempt %d: no HELLO reply", attempt);
            continue;
        }
        out->updater_reachable = true;
        out->last_error[0] = '\0';
        strlcpy(out->installed_version, h.boot_slot_version, sizeof(out->installed_version));
        out->installed_valid = out->installed_version[0] != '\0'; // an empty version is a blank boot slot
        strlcpy(out->updater_version, h.updater_version, sizeof(out->updater_version));
    }
}

static bool jumper_seen(const rcp_sync_link_t *link)
{
    return link->jumper_present && link->jumper_present();
}

esp_err_t rcp_sync_probe_with(const rcp_sync_link_t *link, rcp_sync_probe_result_t *out)
{
    if (!link || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    sync_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        link->release();
        return ESP_ERR_NO_MEM;
    }
    ctx->link = link;

    probe_updater(ctx, out);
    out->jumper_present = jumper_seen(link); // the banner shows during the reset of an updater entry

    const char *base = link->base_version ? link->base_version() : NULL;
    const rcp_sync_recovery_input_t input = {
        .jumper_present = out->jumper_present,
        .updater_reachable = out->updater_reachable,
        .updater_version = out->updater_version,
        .base_version = base,
        .already_attempted = s_auto_recovery_done,
    };
    if (link->rom_recover && rcp_sync_recovery_wanted(&input)) {
        s_auto_recovery_done = true;
        ESP_LOGW(TAG, "ROM recovery of the C6 base image (%s)",
                 out->updater_reachable ? "updater version differs from the embedded one" : "updater not responding");
        link->release();
        const esp_err_t err = link->rom_recover();
        if (err == ESP_OK) {
            memset(out, 0, sizeof(*out));
            out->rom_recovered = true;
            probe_updater(ctx, out);
            out->jumper_present = jumper_seen(link);
        } else {
            ESP_LOGE(TAG, "ROM recovery failed: %s", esp_err_to_name(err));
            // The probe fields stay as they were before the attempt: a reachable updater with an RCP is still what the
            // C6 is believed to run (br_core decides what a failure means from updater_reachable).
            out->rom_recovery_failed = true;
            if (!out->updater_reachable) {
                strlcpy(out->last_error, "rom recovery failed", sizeof(out->last_error));
            }
        }
    }

    link->release();
    free(ctx);
    portENTER_CRITICAL(&s_last_lock);
    s_last = *out;
    portEXIT_CRITICAL(&s_last_lock);
    return ESP_OK;
}

esp_err_t rcp_sync_rom_recover_with(const rcp_sync_link_t *link)
{
    if (!link || !link->rom_recover) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    link->release(); // the flasher takes the C6 pins and resets it
    return link->rom_recover();
}

void rcp_sync_recovery_guard_reset(void)
{
    s_auto_recovery_done = false;
}

esp_err_t rcp_sync_push_firmware_with(const rcp_sync_link_t *link, const rcp_fw_source_t *source, char *err_detail,
                                      size_t err_cap)
{
    set_text(err_detail, err_cap, "");
    if (!link || !source || !source->read) {
        return ESP_ERR_INVALID_ARG;
    }
    sync_ctx_t *ctx = calloc(1, sizeof(*ctx));
    uint8_t *chunk = malloc(DATA_OFFSET_LEN + CHUNK_MAX);
    if (!ctx || !chunk) {
        free(ctx);
        free(chunk);
        link->release();
        set_text(err_detail, err_cap, "out of memory");
        return ESP_ERR_NO_MEM;
    }
    ctx->link = link;

    const int attempts = source->reopen ? PUSH_ATTEMPTS : 1;
    esp_err_t result = ESP_FAIL;
    for (int attempt = 1; attempt <= attempts; attempt++) {
        if (attempt > 1) {
            result = source->reopen(source->ctx);
            if (result != ESP_OK) {
                set_text(err_detail, err_cap, "reopening the firmware source failed");
                break;
            }
        }
        rcp_sync_hello_reply_t h;
        if (link->enter_updater() != ESP_OK || hello(ctx, &h) != ESP_OK) {
            ESP_LOGW(TAG, "attempt %d: no HELLO reply", attempt);
            set_text(err_detail, err_cap, "updater not responding");
            result = ESP_ERR_TIMEOUT;
            continue;
        }
        if (source->total_size == 0 || source->total_size > h.slot_size) {
            ESP_LOGE(TAG, "image size %u does not fit the C6 slot (%u)", (unsigned)source->total_size,
                     (unsigned)h.slot_size);
            set_text(err_detail, err_cap, "image size is 0 or exceeds the C6 slot");
            result = ESP_ERR_INVALID_SIZE;
            break;
        }
        attempt_end_t end;
        result = transfer(ctx, source, chunk, err_detail, err_cap, &end);
        if (result == ESP_OK) {
            set_text(err_detail, err_cap, "");
            break;
        }
        ESP_LOGW(TAG, "attempt %d: transfer failed (%s)", attempt, esp_err_to_name(result));
        if (end == ATTEMPT_FINAL) {
            break;
        }
    }

    link->release();
    free(chunk);
    free(ctx);
    return result;
}

void rcp_sync_get_last_probe(rcp_sync_probe_result_t *out)
{
    portENTER_CRITICAL(&s_last_lock);
    *out = s_last;
    portEXIT_CRITICAL(&s_last_lock);
}
