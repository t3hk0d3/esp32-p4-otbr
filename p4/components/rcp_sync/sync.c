#include <stdio.h>
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
/* After the ESP-Hosted OTA: HELLOs until the updater answers. Each silent HELLO costs EXCHANGE_TRIES link timeouts
 * (about 3 s on the device), so this is about 90 s: OTA restart (5 s), relocation, install and the updater's boot. */
#define WAIT_UPDATER_HELLOS 30

/* esp_app_desc_t inside an app image: after esp_image_header_t (24 bytes) and the first segment header (8 bytes). */
#define APP_DESC_OFFSET     32
#define APP_DESC_MAGIC      0xABCD5432u
#define APP_DESC_VERSION    16 /* char version[32] */
#define APP_DESC_PROJECT    48 /* char project_name[32] */
#define APP_DESC_FIELD_LEN  32
#define IMAGE_MAGIC         0xE9
#define IMAGE_HEAD_LEN      (APP_DESC_OFFSET + APP_DESC_PROJECT + APP_DESC_FIELD_LEN)

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
/* The last push installed c6_installer.bin (rcp_sync_last_push_was_installer). Written by the push only. */
static bool s_last_push_installer;

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

/* Copy a fixed-size, possibly unterminated text field of the app description. */
static void desc_field(const uint8_t *p, char *out)
{
    memcpy(out, p, APP_DESC_FIELD_LEN);
    out[APP_DESC_FIELD_LEN] = '\0';
}

/* Project name and version from the app description at the start of an image. False when it is no app image. */
static bool image_desc(const uint8_t *img, size_t len, char project[APP_DESC_FIELD_LEN + 1],
                       char version[APP_DESC_FIELD_LEN + 1])
{
    if (len < IMAGE_HEAD_LEN || img[0] != IMAGE_MAGIC || get_u32(img + APP_DESC_OFFSET) != APP_DESC_MAGIC) {
        return false;
    }
    desc_field(img + APP_DESC_OFFSET + APP_DESC_PROJECT, project);
    desc_field(img + APP_DESC_OFFSET + APP_DESC_VERSION, version);
    return true;
}

static bool is_installer(const uint8_t *img, size_t len)
{
    char project[APP_DESC_FIELD_LEN + 1], version[APP_DESC_FIELD_LEN + 1];
    return image_desc(img, len, project, version) && strcmp(project, RCP_SYNC_INSTALLER_PROJECT) == 0;
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
    // The first chunk is read before BEGIN: an installer image is refused before the C6 erases anything.
    const size_t first = size < CHUNK_MAX ? size : CHUNK_MAX;
    esp_err_t err = source->read(source->ctx, 0, chunk + DATA_OFFSET_LEN, first);
    if (err != ESP_OK) {
        set_text(detail, cap, "reading the firmware source failed");
        return err;
    }
    if (is_installer(chunk + DATA_OFFSET_LEN, first)) {
        set_text(detail, cap, "c6_installer.bin is for a factory C6; upload c6_rcp.bin");
        *end = ATTEMPT_FINAL;
        return RCP_SYNC_ERR_IMAGE_REJECTED;
    }

    put_u32(size_payload, (uint32_t)size);
    ESP_LOGI(TAG, "BEGIN %u bytes", (unsigned)size);
    err = exchange(ctx, RCP_SYNC_MSG_BEGIN, size_payload, sizeof(size_payload), BEGIN_TIMEOUT_MS, &reply);
    if (err != ESP_OK) {
        set_text(detail, cap, "C6 refused or did not answer BEGIN");
        return err;
    }

    size_t offset = 0;
    while (offset < size) {
        size_t n = size - offset < CHUNK_MAX ? size - offset : CHUNK_MAX;
        put_u32(chunk, (uint32_t)offset);
        if (offset > 0) { // the first chunk is in place already
            err = source->read(source->ctx, offset, chunk + DATA_OFFSET_LEN, n);
            if (err != ESP_OK) {
                set_text(detail, cap, "reading the firmware source failed");
                return err;
            }
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

/* The factory ESP-Hosted firmware answers on SDIO. Releases the SPI link first (the same lines). */
static bool esp_hosted_answers(const rcp_sync_link_t *link)
{
    if (!link->hosted_detect || !link->hosted_ota || !link->attach_updater) {
        return false;
    }
    link->release();
    bool present = false;
    const esp_err_t err = link->hosted_detect(&present);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "looking for ESP-Hosted failed: %s", esp_err_to_name(err));
    }
    return err == ESP_OK && present;
}

/* The source as it was, with its first head_len bytes already read into head (a one-shot source cannot reread them). */
typedef struct {
    const rcp_fw_source_t *source;
    const uint8_t *head;
    size_t head_len;
} replay_t;

static esp_err_t replay_read(void *arg, size_t offset, void *buf, size_t len)
{
    const replay_t *r = arg;
    uint8_t *out = buf;

    if (offset < r->head_len) {
        const size_t n = r->head_len - offset < len ? r->head_len - offset : len;
        memcpy(out, r->head + offset, n);
        offset += n;
        out += n;
        len -= n;
    }
    return len ? r->source->read(r->source->ctx, offset, out, len) : ESP_OK;
}

/* Wait for the updater without resetting the C6: it boots into the updater once the installer is through. */
static esp_err_t wait_updater(sync_ctx_t *ctx)
{
    esp_err_t err = ctx->link->attach_updater();
    for (int i = 0; err == ESP_OK && i < WAIT_UPDATER_HELLOS; i++) {
        rcp_sync_hello_reply_t h;
        if (hello(ctx, &h) == ESP_OK) {
            ESP_LOGI(TAG, "updater %s answers after the installer", h.updater_version);
            return ESP_OK;
        }
    }
    return err == ESP_OK ? ESP_ERR_TIMEOUT : err;
}

/* First install: c6_installer.bin of this release through the ESP-Hosted OTA, then wait for the updater. */
static esp_err_t push_installer(sync_ctx_t *ctx, const rcp_fw_source_t *source, uint8_t *head, char *detail, size_t cap)
{
    const rcp_sync_link_t *link = ctx->link;
    char project[APP_DESC_FIELD_LEN + 1], version[APP_DESC_FIELD_LEN + 1];
    char text[160];
    const size_t head_len = source->total_size < IMAGE_HEAD_LEN ? source->total_size : IMAGE_HEAD_LEN;

    esp_err_t err = source->read(source->ctx, 0, head, head_len);
    if (err != ESP_OK) {
        set_text(detail, cap, "reading the firmware source failed");
        return err;
    }
    if (!image_desc(head, head_len, project, version) || strcmp(project, RCP_SYNC_INSTALLER_PROJECT) != 0) {
        set_text(detail, cap, "C6 runs factory ESP-Hosted: upload c6_installer.bin first");
        return RCP_SYNC_ERR_IMAGE_REJECTED;
    }
    const char *base = link->base_version ? link->base_version() : NULL;
    if (!base || strcmp(version, base) != 0) {
        snprintf(text, sizeof(text), "wrong c6_installer.bin: %s, need %s", version,
                 base ? base : "none");
        set_text(detail, cap, text);
        return RCP_SYNC_ERR_IMAGE_REJECTED;
    }

    ESP_LOGW(TAG, "sending c6_installer %s through the ESP-Hosted OTA", version);
    const replay_t replay = {.source = source, .head = head, .head_len = head_len};
    const rcp_fw_source_t through = {.total_size = source->total_size, .read = replay_read, .ctx = (void *) &replay};
    err = link->hosted_ota(&through, detail, cap);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ESP-Hosted OTA failed: %s (%s)", esp_err_to_name(err), detail ? detail : "");
        return RCP_SYNC_ERR_HOSTED_OTA;
    }
    err = wait_updater(ctx);
    if (err != ESP_OK) {
        set_text(detail, cap, "the C6 did not come back after the installer");
        return err;
    }
    s_last_push_installer = true;
    return ESP_OK;
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
    if (!out->updater_reachable && !out->jumper_present && esp_hosted_answers(link)) {
        out->esp_hosted = true;
        strlcpy(out->last_error, RCP_SYNC_ESP_HOSTED_ERROR, sizeof(out->last_error));
    }

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
    s_last_push_installer = false;
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
            if (attempt == 1 && !jumper_seen(link) && esp_hosted_answers(link)) {
                result = push_installer(ctx, source, chunk, err_detail, err_cap);
                break;
            }
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

bool rcp_sync_last_push_was_installer(void)
{
    return s_last_push_installer;
}
