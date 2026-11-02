#include <string.h>
#include "hosted_frame.h"

#define EP_TYPE   0x01
#define DATA_TYPE 0x02
#define EP_RSP    "RPCRsp"
#define EP_EVT    "RPCEvt"
#define EP_LEN    6

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

size_t hosted_frame_encode(uint8_t if_type, uint16_t seq, const uint8_t *payload, uint16_t len, uint8_t *out, size_t cap)
{
    const size_t total = HOSTED_HEADER_LEN + (size_t)len;

    if (!out || (!payload && len) || total > cap || total > HOSTED_MAX_PACKET) {
        return 0;
    }
    memset(out, 0, HOSTED_HEADER_LEN);
    out[0] = (uint8_t)(if_type & 0x0F); // if_type:4 in the low bits, if_num:4 (0) above
    put_u16(out + 2, len);
    put_u16(out + 4, HOSTED_HEADER_LEN);
    put_u16(out + 8, seq);
    if (len) {
        memcpy(out + HOSTED_HEADER_LEN, payload, len);
    }
    uint16_t sum = 0;
    for (size_t i = 0; i < total; i++) {
        sum += out[i];
    }
    put_u16(out + 6, sum);
    return total;
}

int hosted_frame_next(const uint8_t *buf, size_t len, hosted_packet_t *out)
{
    if (len < HOSTED_HEADER_LEN) {
        return 0;
    }
    const uint16_t plen = get_u16(buf + 2);
    const uint16_t offset = get_u16(buf + 4);
    if (offset < HOSTED_HEADER_LEN || (size_t)offset + plen > HOSTED_MAX_PACKET) {
        return -1; // also all-zero block padding
    }
    if ((size_t)offset + plen > len) {
        return 0;
    }
    out->if_type = buf[0] & 0x0F;
    out->if_num = buf[0] >> 4;
    out->flags = buf[1];
    out->seq = get_u16(buf + 8);
    out->pkt_type = buf[11];
    out->payload = buf + offset;
    out->len = plen;
    return (int)(offset + plen);
}

size_t hosted_serial_wrap(const uint8_t *proto, size_t len, uint8_t *out, size_t cap)
{
    const size_t total = HOSTED_SERIAL_OVERHEAD + len;

    if (!out || (!proto && len) || total > cap || len > 0xFFFF) {
        return 0;
    }
    out[0] = EP_TYPE;
    put_u16(out + 1, EP_LEN);
    memcpy(out + 3, EP_RSP, EP_LEN);
    out[3 + EP_LEN] = DATA_TYPE;
    put_u16(out + 4 + EP_LEN, (uint16_t)len);
    if (len) {
        memcpy(out + HOSTED_SERIAL_OVERHEAD, proto, len);
    }
    return total;
}

esp_err_t hosted_serial_unwrap(const uint8_t *buf, size_t len, bool *is_event, const uint8_t **proto, size_t *proto_len)
{
    if (!buf || len < HOSTED_SERIAL_OVERHEAD || buf[0] != EP_TYPE || get_u16(buf + 1) != EP_LEN ||
            buf[3 + EP_LEN] != DATA_TYPE) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    const bool rsp = memcmp(buf + 3, EP_RSP, EP_LEN) == 0;
    const bool evt = memcmp(buf + 3, EP_EVT, EP_LEN) == 0;
    const size_t n = get_u16(buf + 4 + EP_LEN);
    if ((!rsp && !evt) || HOSTED_SERIAL_OVERHEAD + n > len) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    *is_event = evt;
    *proto = buf + HOSTED_SERIAL_OVERHEAD;
    *proto_len = n;
    return ESP_OK;
}

esp_err_t hosted_priv_init_chip_id(const hosted_packet_t *pkt, uint8_t *chip_id)
{
    // struct esp_priv_event { uint8_t event_type; uint8_t event_len; uint8_t event_data[]; }: TLVs of tag, length, value.
    if (pkt->if_type != HOSTED_IF_PRIV || pkt->pkt_type != HOSTED_PRIV_PKT_EVENT || pkt->len < 2 ||
            pkt->payload[0] != HOSTED_PRIV_EVENT_INIT) {
        return ESP_ERR_NOT_FOUND;
    }
    size_t end = 2 + (size_t)pkt->payload[1];
    if (end > pkt->len) {
        end = pkt->len;
    }
    for (size_t i = 2; i + 2 <= end;) {
        const uint8_t tag = pkt->payload[i];
        const uint8_t tlen = pkt->payload[i + 1];
        if (i + 2 + tlen > end) {
            break;
        }
        if (tag == HOSTED_PRIV_TAG_CHIP_ID && tlen >= 1) {
            *chip_id = pkt->payload[i + 2];
            return ESP_OK;
        }
        i += 2 + (size_t)tlen;
    }
    return ESP_ERR_NOT_FOUND;
}
