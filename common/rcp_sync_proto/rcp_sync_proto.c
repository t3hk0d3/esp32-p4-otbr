#include <string.h>
#include "sdkconfig.h"
#include "esp_err.h"
#include "rcp_sync_proto.h"

/* Rejecting malformed input is routine on the wire, so these checks stay silent (no logging). */
#define RETURN_UNLESS(cond, err) do { if (!(cond)) { return (err); } } while (0)

#if CONFIG_IDF_TARGET_LINUX
/* Same polynomial and conventions as esp_rom_crc32_le (reflected 0xEDB88320, init and xor-out 0xFFFFFFFF). */
static uint32_t crc32_le(uint32_t crc, const uint8_t *buf, uint32_t len)
{
    crc = ~crc;
    while (len--) {
        crc ^= *buf++;
        for (int i = 0; i < 8; i++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}
#else
#include "esp_rom_crc.h"
#define crc32_le esp_rom_crc32_le
#endif

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

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)get_u16(p) | ((uint32_t)get_u16(p + 2) << 16);
}

esp_err_t rcp_sync_proto_encode(const rcp_sync_proto_frame_t *frame, uint8_t *buf, size_t cap, size_t *out_len)
{
    RETURN_UNLESS(frame && buf && out_len, ESP_ERR_INVALID_ARG);
    RETURN_UNLESS(frame->len == 0 || frame->payload, ESP_ERR_INVALID_ARG);
    RETURN_UNLESS(frame->len <= RCP_SYNC_PROTO_MAX_PAYLOAD, ESP_ERR_INVALID_SIZE);
    size_t total = RCP_SYNC_PROTO_HEADER_LEN + frame->len + RCP_SYNC_PROTO_CRC_LEN;
    RETURN_UNLESS(cap >= total, ESP_ERR_INVALID_SIZE);

    put_u16(buf, RCP_SYNC_PROTO_MAGIC);
    buf[2] = frame->type;
    buf[3] = frame->seq;
    put_u16(buf + 4, frame->len);
    if (frame->len) {
        memcpy(buf + RCP_SYNC_PROTO_HEADER_LEN, frame->payload, frame->len);
    }
    size_t body = RCP_SYNC_PROTO_HEADER_LEN + frame->len;
    put_u32(buf + body, crc32_le(0, buf, body));
    *out_len = total;
    return ESP_OK;
}

esp_err_t rcp_sync_proto_decode(const uint8_t *buf, size_t len, rcp_sync_proto_frame_t *out)
{
    RETURN_UNLESS(buf && out, ESP_ERR_INVALID_ARG);
    RETURN_UNLESS(len >= RCP_SYNC_PROTO_HEADER_LEN + RCP_SYNC_PROTO_CRC_LEN, ESP_ERR_INVALID_SIZE);
    RETURN_UNLESS(get_u16(buf) == RCP_SYNC_PROTO_MAGIC, ESP_ERR_INVALID_RESPONSE);
    uint16_t plen = get_u16(buf + 4);
    RETURN_UNLESS(plen <= RCP_SYNC_PROTO_MAX_PAYLOAD, ESP_ERR_INVALID_SIZE);
    size_t body = RCP_SYNC_PROTO_HEADER_LEN + plen;
    RETURN_UNLESS(len >= body + RCP_SYNC_PROTO_CRC_LEN, ESP_ERR_INVALID_SIZE);
    RETURN_UNLESS(get_u32(buf + body) == crc32_le(0, buf, body), ESP_ERR_INVALID_CRC);

    out->type = buf[2];
    out->seq = buf[3];
    out->len = plen;
    out->payload = buf + RCP_SYNC_PROTO_HEADER_LEN;
    return ESP_OK;
}

static void put_str(uint8_t *p, const char *s)
{
    size_t n = strnlen(s, RCP_SYNC_PROTO_VERSION_STR_LEN - 1);
    memset(p, 0, RCP_SYNC_PROTO_VERSION_STR_LEN);
    memcpy(p, s, n);
}

static void get_str(char *dst, const uint8_t *p)
{
    memcpy(dst, p, RCP_SYNC_PROTO_VERSION_STR_LEN);
    dst[RCP_SYNC_PROTO_VERSION_STR_LEN - 1] = '\0';
}

esp_err_t rcp_sync_proto_put_hello_reply(const rcp_sync_hello_reply_t *reply, uint8_t *buf, size_t cap, size_t *out_len)
{
    RETURN_UNLESS(reply && buf && out_len, ESP_ERR_INVALID_ARG);
    RETURN_UNLESS(cap >= RCP_SYNC_PROTO_HELLO_REPLY_LEN, ESP_ERR_INVALID_SIZE);
    uint8_t *p = buf;
    put_u16(p, reply->proto_version);
    p += 2;
    put_str(p, reply->updater_version);
    p += RCP_SYNC_PROTO_VERSION_STR_LEN;
    put_str(p, reply->boot_slot_version);
    p += RCP_SYNC_PROTO_VERSION_STR_LEN;
    put_str(p, reply->other_slot_version);
    p += RCP_SYNC_PROTO_VERSION_STR_LEN;
    put_u32(p, reply->slot_size);
    *out_len = RCP_SYNC_PROTO_HELLO_REPLY_LEN;
    return ESP_OK;
}

esp_err_t rcp_sync_proto_get_hello_reply(const uint8_t *buf, size_t len, rcp_sync_hello_reply_t *out)
{
    RETURN_UNLESS(buf && out, ESP_ERR_INVALID_ARG);
    RETURN_UNLESS(len >= RCP_SYNC_PROTO_HELLO_REPLY_LEN, ESP_ERR_INVALID_SIZE);
    const uint8_t *p = buf;
    out->proto_version = get_u16(p);
    p += 2;
    get_str(out->updater_version, p);
    p += RCP_SYNC_PROTO_VERSION_STR_LEN;
    get_str(out->boot_slot_version, p);
    p += RCP_SYNC_PROTO_VERSION_STR_LEN;
    get_str(out->other_slot_version, p);
    p += RCP_SYNC_PROTO_VERSION_STR_LEN;
    out->slot_size = get_u32(p);
    return ESP_OK;
}
