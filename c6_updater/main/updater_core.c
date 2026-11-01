#include <string.h>
#include "updater_core.h"

#define PROTO_VERSION_ON_WIRE 1
#define ERR_PAYLOAD_LEN       6 /* u16 code + u32 expected_offset */

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put_u32(uint8_t *p, uint32_t v)
{
    put_u16(p, (uint16_t)v);
    put_u16(p + 2, (uint16_t)(v >> 16));
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static esp_err_t encode_reply(uint8_t type, uint8_t seq, const uint8_t *payload, uint16_t len,
                              uint8_t *reply, size_t reply_cap, size_t *reply_len)
{
    const rcp_sync_proto_frame_t f = {.type = type, .seq = seq, .len = len, .payload = payload};
    return rcp_sync_proto_encode(&f, reply, reply_cap, reply_len);
}

static esp_err_t reply_err(uint8_t seq, uint16_t code, uint32_t expected_offset,
                           uint8_t *reply, size_t reply_cap, size_t *reply_len)
{
    uint8_t payload[ERR_PAYLOAD_LEN];

    put_u16(payload, code);
    put_u32(&payload[2], expected_offset);
    return encode_reply(RCP_SYNC_MSG_ERR, seq, payload, sizeof(payload), reply, reply_cap, reply_len);
}

static esp_err_t handle_hello(updater_core_t *ctx, uint8_t seq, uint8_t *reply, size_t reply_cap, size_t *reply_len)
{
    rcp_sync_hello_reply_t h = {.proto_version = PROTO_VERSION_ON_WIRE, .slot_size = ctx->flash->slot_size};
    uint8_t payload[RCP_SYNC_PROTO_HELLO_REPLY_LEN];
    size_t payload_len = 0;

    strlcpy(h.updater_version, ctx->updater_version, sizeof(h.updater_version));
    if (ctx->flash->slot_version(true, h.boot_slot_version, sizeof(h.boot_slot_version)) != ESP_OK) {
        h.boot_slot_version[0] = '\0';
    }
    if (ctx->flash->slot_version(false, h.other_slot_version, sizeof(h.other_slot_version)) != ESP_OK) {
        h.other_slot_version[0] = '\0';
    }
    esp_err_t err = rcp_sync_proto_put_hello_reply(&h, payload, sizeof(payload), &payload_len);
    if (err != ESP_OK) {
        return err;
    }
    return encode_reply(RCP_SYNC_MSG_OK, seq, payload, (uint16_t)payload_len, reply, reply_cap, reply_len);
}

static esp_err_t handle_begin(updater_core_t *ctx, const rcp_sync_proto_frame_t *req,
                              uint8_t *reply, size_t reply_cap, size_t *reply_len)
{
    if (req->len != 4) {
        return reply_err(req->seq, RCP_SYNC_ERR_STATE, 0, reply, reply_cap, reply_len);
    }
    uint32_t image_size = get_u32(req->payload);
    if (image_size == 0 || image_size > ctx->flash->slot_size) {
        return reply_err(req->seq, RCP_SYNC_ERR_SIZE, 0, reply, reply_cap, reply_len);
    }
    ctx->receiving = false;
    if (ctx->flash->begin(image_size) != ESP_OK) {
        return reply_err(req->seq, RCP_SYNC_ERR_FLASH, 0, reply, reply_cap, reply_len);
    }
    ctx->receiving = true;
    ctx->image_size = image_size;
    ctx->next_offset = 0;
    return encode_reply(RCP_SYNC_MSG_OK, req->seq, NULL, 0, reply, reply_cap, reply_len);
}

static esp_err_t handle_data(updater_core_t *ctx, const rcp_sync_proto_frame_t *req,
                             uint8_t *reply, size_t reply_cap, size_t *reply_len)
{
    if (!ctx->receiving || req->len < 4) {
        return reply_err(req->seq, RCP_SYNC_ERR_STATE, 0, reply, reply_cap, reply_len);
    }
    uint32_t offset = get_u32(req->payload);
    size_t n = req->len - 4u;

    if (offset != ctx->next_offset) {
        return reply_err(req->seq, RCP_SYNC_ERR_OFFSET, ctx->next_offset, reply, reply_cap, reply_len);
    }
    if (n > ctx->image_size - offset) {
        return reply_err(req->seq, RCP_SYNC_ERR_SIZE, ctx->next_offset, reply, reply_cap, reply_len);
    }
    if (n > 0 && ctx->flash->write(offset, req->payload + 4, n) != ESP_OK) {
        return reply_err(req->seq, RCP_SYNC_ERR_FLASH, ctx->next_offset, reply, reply_cap, reply_len);
    }
    ctx->next_offset += (uint32_t)n;

    uint8_t payload[4];
    put_u32(payload, ctx->next_offset);
    return encode_reply(RCP_SYNC_MSG_OK, req->seq, payload, sizeof(payload), reply, reply_cap, reply_len);
}

static esp_err_t handle_end(updater_core_t *ctx, const rcp_sync_proto_frame_t *req,
                            uint8_t *reply, size_t reply_cap, size_t *reply_len)
{
    if (!ctx->receiving || ctx->next_offset != ctx->image_size) {
        return reply_err(req->seq, RCP_SYNC_ERR_STATE, ctx->next_offset, reply, reply_cap, reply_len);
    }
    ctx->receiving = false;
    if (ctx->flash->finish() != ESP_OK) {
        return reply_err(req->seq, RCP_SYNC_ERR_VERIFY, 0, reply, reply_cap, reply_len);
    }
    return encode_reply(RCP_SYNC_MSG_OK, req->seq, NULL, 0, reply, reply_cap, reply_len);
}

int updater_core_choose_boot_slot(updater_otadata_t selected, bool valid0, bool valid1)
{
    const bool valid[2] = {valid0, valid1};
    const int first = (selected == UPDATER_OTADATA_SLOT_1) ? 1 : 0;

    if (valid[first]) {
        return first;
    }
    if (valid[1 - first]) {
        return 1 - first;
    }
    return first;
}

void updater_core_init(updater_core_t *ctx, const updater_flash_t *flash, const char *updater_version)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->flash = flash;
    ctx->updater_version = updater_version;
}

esp_err_t updater_core_handle(updater_core_t *ctx, const uint8_t *req, size_t req_len,
                              uint8_t *reply, size_t reply_cap, size_t *reply_len)
{
    if (!ctx || !req || !reply || !reply_len) {
        return ESP_ERR_INVALID_ARG;
    }
    *reply_len = 0;

    rcp_sync_proto_frame_t f;
    esp_err_t err = rcp_sync_proto_decode(req, req_len, &f);
    if (err != ESP_OK) {
        /* CRC mismatch, truncation, bad magic: the seq byte cannot be trusted, so NAK with seq 0.
         * The P4 resends the same request on NAK. */
        return encode_reply(RCP_SYNC_MSG_NAK, 0, NULL, 0, reply, reply_cap, reply_len);
    }

    /* Replay only an exact retransmit (same seq, type, len and CRC). HELLO is never replayed. */
    uint32_t req_crc = get_u32(req + RCP_SYNC_PROTO_HEADER_LEN + f.len);
    if (f.type != RCP_SYNC_MSG_HELLO && ctx->have_last && f.seq == ctx->last_seq &&
            f.type == ctx->last_type && f.len == ctx->last_len && req_crc == ctx->last_crc) {
        if (ctx->cache_len > reply_cap) {
            return ESP_ERR_INVALID_SIZE;
        }
        memcpy(reply, ctx->cache, ctx->cache_len);
        *reply_len = ctx->cache_len;
        return ESP_OK;
    }

    switch (f.type) {
    case RCP_SYNC_MSG_HELLO:
        err = handle_hello(ctx, f.seq, reply, reply_cap, reply_len);
        break;
    case RCP_SYNC_MSG_BEGIN:
        err = handle_begin(ctx, &f, reply, reply_cap, reply_len);
        break;
    case RCP_SYNC_MSG_DATA:
        err = handle_data(ctx, &f, reply, reply_cap, reply_len);
        break;
    case RCP_SYNC_MSG_END:
        err = handle_end(ctx, &f, reply, reply_cap, reply_len);
        break;
    default:
        err = reply_err(f.seq, RCP_SYNC_ERR_STATE, 0, reply, reply_cap, reply_len);
        break;
    }
    if (err == ESP_OK && f.type != RCP_SYNC_MSG_HELLO && *reply_len <= sizeof(ctx->cache)) {
        memcpy(ctx->cache, reply, *reply_len);
        ctx->cache_len = *reply_len;
        ctx->last_seq = f.seq;
        ctx->last_type = f.type;
        ctx->last_len = f.len;
        ctx->last_crc = req_crc;
        ctx->have_last = true;
    }
    return err;
}
