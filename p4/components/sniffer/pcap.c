#include "sniffer.h"

#include <string.h>

#define PCAP_MAGIC 0xa1b2c3d4u
#define PCAP_RECORD_HEADER_LEN 16
#define TAP_TLV_FCS_TYPE 0
#define TAP_TLV_RSS 1
#define TAP_TLV_CHANNEL 3
#define TAP_TLV_LQI 10
#define TAP_FCS_NONE 0
#define TAP_FCS_CRC16 1
#define FCS_LEN 2

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

// One TLV: type, length, the value zero-padded to 4 bytes. Returns the bytes written.
static size_t put_tlv(uint8_t *p, uint16_t type, const uint8_t *value, uint16_t len)
{
    const size_t padded = (len + 3u) & ~3u;

    put_u16(p, type);
    put_u16(p + 2, len);
    memset(p + 4, 0, padded);
    memcpy(p + 4, value, len);
    return 4 + padded;
}

size_t sniffer_pcap_global_header(uint8_t *buf, size_t cap)
{
    if (cap < SNIFFER_PCAP_GLOBAL_HEADER_LEN) {
        return 0;
    }
    put_u32(buf, PCAP_MAGIC);
    put_u16(buf + 4, 2);
    put_u16(buf + 6, 4);
    put_u32(buf + 8, 0);  // thiszone
    put_u32(buf + 12, 0); // sigfigs
    put_u32(buf + 16, SNIFFER_PCAP_SNAPLEN);
    put_u32(buf + 20, SNIFFER_PCAP_LINKTYPE_IEEE802_15_4_TAP);
    return SNIFFER_PCAP_GLOBAL_HEADER_LEN;
}

size_t sniffer_pcap_record(const sniffer_frame_t *f, bool fcs_valid, uint8_t *buf, size_t cap)
{
    if (f->psdu_len > SNIFFER_MAX_PSDU_LEN || (!fcs_valid && f->psdu_len < FCS_LEN)) {
        return 0;
    }
    const size_t psdu_len = fcs_valid ? f->psdu_len : f->psdu_len - FCS_LEN;
    const size_t incl_len = SNIFFER_TAP_HEADER_LEN + psdu_len;

    if (cap < PCAP_RECORD_HEADER_LEN + incl_len) {
        return 0;
    }
    put_u32(buf, (uint32_t)(f->timestamp_us / 1000000u));
    put_u32(buf + 4, (uint32_t)(f->timestamp_us % 1000000u));
    put_u32(buf + 8, (uint32_t)incl_len);
    put_u32(buf + 12, (uint32_t)incl_len);

    uint8_t *tap = buf + PCAP_RECORD_HEADER_LEN;
    uint8_t *p = tap;
    tap[0] = 0; // TAP version
    tap[1] = 0;
    put_u16(tap + 2, SNIFFER_TAP_HEADER_LEN);
    p += 4;

    const uint8_t fcs_type = fcs_valid ? TAP_FCS_CRC16 : TAP_FCS_NONE;
    p += put_tlv(p, TAP_TLV_FCS_TYPE, &fcs_type, 1);

    const float rss = (float)f->rssi;
    uint8_t rss_bytes[4];
    uint32_t rss_bits;
    memcpy(&rss_bits, &rss, sizeof(rss_bits));
    put_u32(rss_bytes, rss_bits);
    p += put_tlv(p, TAP_TLV_RSS, rss_bytes, sizeof(rss_bytes));

    const uint8_t channel[3] = {f->channel, 0, 0}; // channel u16 little-endian, page 0
    p += put_tlv(p, TAP_TLV_CHANNEL, channel, sizeof(channel));
    p += put_tlv(p, TAP_TLV_LQI, &f->lqi, 1);

    memcpy(p, f->psdu, psdu_len);
    return PCAP_RECORD_HEADER_LEN + incl_len;
}
