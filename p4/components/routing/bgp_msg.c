// BGP message codec (spec section 5): pure and host-tested against scapy-built vectors (tools/routing_vectors.py).
#include "bgp_msg.h"

#include <string.h>

#define MARKER_LEN 16
#define OPEN_MIN_LEN 29 /* header, version, My AS, hold time, BGP identifier, optional parameters length */
#define UPDATE_MIN_LEN 23 /* header, withdrawn routes length, total path attribute length */
#define NOTIFICATION_MIN_LEN 21
#define AS_TRANS 23456
#define PREFIX64_NLRI_LEN 9 /* prefix length byte + 8 bytes */
#define LOCAL_PREF_DEFAULT 100

/* NOTIFICATION codes and subcodes (RFC 4271 section 4.5 and 6) */
#define ERR_HEADER 1
#define ERR_HEADER_SYNC 1
#define ERR_HEADER_LENGTH 2
#define ERR_HEADER_TYPE 3
#define ERR_OPEN 2
#define ERR_OPEN_UNSPECIFIC 0
#define ERR_OPEN_VERSION 1
#define ERR_OPEN_PEER_AS 2
#define ERR_OPEN_BGP_ID 3
#define ERR_OPEN_OPT_PARAM 4
#define ERR_OPEN_HOLD_TIME 6
#define ERR_OPEN_CAPABILITY 7 /* RFC 5492 */
#define ERR_UPDATE 3
#define ERR_UPDATE_ATTR_LIST 1
#define ERR_UPDATE_OPT_ATTR 9
#define ERR_UPDATE_NETWORK 10

#define OPT_PARAM_CAPABILITIES 2
#define CAP_MULTIPROTOCOL 1
#define CAP_AS4 65
#define AFI_IPV6 2
#define SAFI_UNICAST 1

/* path attributes (RFC 4271 section 5, RFC 4760) */
#define ATTR_FLAG_OPTIONAL 0x80
#define ATTR_FLAG_TRANSITIVE 0x40
#define ATTR_FLAG_EXT_LEN 0x10
#define ATTR_ORIGIN 1
#define ATTR_AS_PATH 2
#define ATTR_LOCAL_PREF 5
#define ATTR_MP_REACH 14
#define ATTR_MP_UNREACH 15
#define ORIGIN_IGP 0
#define AS_SEQUENCE 2

static const routing_bgp_err_t OK = {0, 0};

static routing_bgp_err_t err(uint8_t code, uint8_t subcode)
{
    routing_bgp_err_t e = {code, subcode};
    return e;
}

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

static uint32_t get32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/* --- encoding --- */

/* A write cursor; the encoders check the total length against cap before writing anything. */
typedef struct {
    uint8_t *p;
} writer_t;

static void put8(writer_t *w, uint8_t v)
{
    *w->p++ = v;
}

static void put16(writer_t *w, uint16_t v)
{
    put8(w, (uint8_t)(v >> 8));
    put8(w, (uint8_t)v);
}

static void put32(writer_t *w, uint32_t v)
{
    put16(w, (uint16_t)(v >> 16));
    put16(w, (uint16_t)v);
}

static void put_bytes(writer_t *w, const uint8_t *src, size_t n)
{
    memcpy(w->p, src, n);
    w->p += n;
}

static void put_header(writer_t *w, uint16_t len, uint8_t type)
{
    memset(w->p, 0xff, MARKER_LEN);
    w->p += MARKER_LEN;
    put16(w, len);
    put8(w, type);
}

/* Attribute header: flags, type, and a 1-byte length, or a 2-byte one with the extended-length flag above 255. */
static size_t attr_header_len(size_t value_len)
{
    return value_len > 0xff ? 4 : 3;
}

static void put_attr_header(writer_t *w, uint8_t flags, uint8_t type, size_t value_len)
{
    if (value_len > 0xff) {
        put8(w, flags | ATTR_FLAG_EXT_LEN);
        put8(w, type);
        put16(w, (uint16_t)value_len);
    } else {
        put8(w, flags);
        put8(w, type);
        put8(w, (uint8_t)value_len);
    }
}

static void put_prefixes(writer_t *w, const routing_prefix_set_t *set)
{
    for (uint8_t i = 0; i < set->count; i++) {
        put8(w, 64);
        put_bytes(w, set->p[i].b, sizeof(set->p[i].b));
    }
}

size_t routing_bgp_encode_open(uint8_t *buf, size_t cap, uint32_t local_asn, uint16_t hold_time, uint32_t router_id)
{
    /* one Capabilities parameter: Multiprotocol (2 + 4) and 4-octet ASN (2 + 4) */
    const size_t caps_len = 12;
    const size_t len = OPEN_MIN_LEN + 2 + caps_len;

    if (len > cap) {
        return 0;
    }
    writer_t w = {buf};
    put_header(&w, (uint16_t)len, ROUTING_BGP_MSG_OPEN);
    put8(&w, 4);
    put16(&w, local_asn > 0xffff ? AS_TRANS : (uint16_t)local_asn);
    put16(&w, hold_time);
    put32(&w, router_id);
    put8(&w, (uint8_t)(2 + caps_len));
    put8(&w, OPT_PARAM_CAPABILITIES);
    put8(&w, (uint8_t)caps_len);
    put8(&w, CAP_MULTIPROTOCOL);
    put8(&w, 4);
    put16(&w, AFI_IPV6);
    put8(&w, 0);
    put8(&w, SAFI_UNICAST);
    put8(&w, CAP_AS4);
    put8(&w, 4);
    put32(&w, local_asn);
    return len;
}

size_t routing_bgp_encode_keepalive(uint8_t *buf, size_t cap)
{
    if (ROUTING_BGP_HDR_LEN > cap) {
        return 0;
    }
    writer_t w = {buf};
    put_header(&w, ROUTING_BGP_HDR_LEN, ROUTING_BGP_MSG_KEEPALIVE);
    return ROUTING_BGP_HDR_LEN;
}

size_t routing_bgp_encode_update(uint8_t *buf, size_t cap, bool ibgp, uint32_t local_asn,
                                 const routing_bgp_nexthop_t *nh, const routing_prefix_set_t *announce)
{
    if (announce->count > ROUTING_MAX_ANNOUNCED) {
        return 0;
    }
    const size_t as_path_len = ibgp ? 0 : 2 + 4; /* empty, or one AS_SEQUENCE of one 4-octet ASN */
    const size_t nh_len = nh->has_global ? 32 : 16;
    /* AFI, SAFI, next-hop length, next-hop, reserved, NLRI */
    const size_t mp_len = 2 + 1 + 1 + nh_len + 1 + (size_t)announce->count * PREFIX64_NLRI_LEN;
    const size_t attrs_len = (3 + 1) + (3 + as_path_len) + (ibgp ? 3 + 4 : 0) + attr_header_len(mp_len) + mp_len;
    const size_t len = UPDATE_MIN_LEN + attrs_len;

    if (len > cap || len > ROUTING_BGP_MAX_MSG) {
        return 0;
    }
    writer_t w = {buf};
    put_header(&w, (uint16_t)len, ROUTING_BGP_MSG_UPDATE);
    put16(&w, 0); /* no withdrawn routes */
    put16(&w, (uint16_t)attrs_len);

    put_attr_header(&w, ATTR_FLAG_TRANSITIVE, ATTR_ORIGIN, 1);
    put8(&w, ORIGIN_IGP);

    put_attr_header(&w, ATTR_FLAG_TRANSITIVE, ATTR_AS_PATH, as_path_len);
    if (!ibgp) {
        put8(&w, AS_SEQUENCE);
        put8(&w, 1);
        put32(&w, local_asn);
    }

    if (ibgp) {
        put_attr_header(&w, ATTR_FLAG_TRANSITIVE, ATTR_LOCAL_PREF, 4);
        put32(&w, LOCAL_PREF_DEFAULT);
    }

    put_attr_header(&w, ATTR_FLAG_OPTIONAL, ATTR_MP_REACH, mp_len);
    put16(&w, AFI_IPV6);
    put8(&w, SAFI_UNICAST);
    put8(&w, (uint8_t)nh_len);
    if (nh->has_global) {
        put_bytes(&w, nh->global, sizeof(nh->global));
    }
    put_bytes(&w, nh->link_local, sizeof(nh->link_local));
    put8(&w, 0); /* reserved */
    put_prefixes(&w, announce);
    return len;
}

size_t routing_bgp_encode_withdraw(uint8_t *buf, size_t cap, const routing_prefix_set_t *withdraw)
{
    if (withdraw->count > ROUTING_MAX_ANNOUNCED) {
        return 0;
    }
    const size_t mp_len = 2 + 1 + (size_t)withdraw->count * PREFIX64_NLRI_LEN; /* AFI, SAFI, withdrawn routes */
    const size_t attrs_len = attr_header_len(mp_len) + mp_len;
    const size_t len = UPDATE_MIN_LEN + attrs_len;

    if (len > cap || len > ROUTING_BGP_MAX_MSG) {
        return 0;
    }
    writer_t w = {buf};
    put_header(&w, (uint16_t)len, ROUTING_BGP_MSG_UPDATE);
    put16(&w, 0);
    put16(&w, (uint16_t)attrs_len);
    put_attr_header(&w, ATTR_FLAG_OPTIONAL, ATTR_MP_UNREACH, mp_len);
    put16(&w, AFI_IPV6);
    put8(&w, SAFI_UNICAST);
    put_prefixes(&w, withdraw);
    return len;
}

size_t routing_bgp_encode_notification(uint8_t *buf, size_t cap, uint8_t code, uint8_t subcode)
{
    if (NOTIFICATION_MIN_LEN > cap) {
        return 0;
    }
    writer_t w = {buf};
    put_header(&w, NOTIFICATION_MIN_LEN, ROUTING_BGP_MSG_NOTIFICATION);
    put8(&w, code);
    put8(&w, subcode);
    return NOTIFICATION_MIN_LEN;
}

/* --- decoding --- */

routing_bgp_err_t routing_bgp_decode_header(const uint8_t *buf, size_t len, uint8_t *type, uint16_t *msg_len)
{
    if (len < ROUTING_BGP_HDR_LEN) {
        return err(ERR_HEADER, ERR_HEADER_LENGTH);
    }
    for (size_t i = 0; i < MARKER_LEN; i++) {
        if (buf[i] != 0xff) {
            return err(ERR_HEADER, ERR_HEADER_SYNC);
        }
    }
    uint16_t n = get16(&buf[MARKER_LEN]);
    uint8_t t = buf[MARKER_LEN + 2];
    if (n < ROUTING_BGP_HDR_LEN || n > ROUTING_BGP_MAX_MSG) {
        return err(ERR_HEADER, ERR_HEADER_LENGTH);
    }
    uint16_t min;
    switch (t) {
    case ROUTING_BGP_MSG_OPEN:
        min = OPEN_MIN_LEN;
        break;
    case ROUTING_BGP_MSG_UPDATE:
        min = UPDATE_MIN_LEN;
        break;
    case ROUTING_BGP_MSG_NOTIFICATION:
        min = NOTIFICATION_MIN_LEN;
        break;
    case ROUTING_BGP_MSG_KEEPALIVE:
        min = ROUTING_BGP_HDR_LEN;
        break;
    default:
        return err(ERR_HEADER, ERR_HEADER_TYPE);
    }
    if (n < min || (t == ROUTING_BGP_MSG_KEEPALIVE && n != ROUTING_BGP_HDR_LEN)) {
        return err(ERR_HEADER, ERR_HEADER_LENGTH);
    }
    *type = t;
    *msg_len = n;
    return OK;
}

/* The capabilities of one Capabilities parameter (RFC 5492); unknown ones are skipped. */
static routing_bgp_err_t parse_caps(const uint8_t *p, size_t len, bool *has_as4, uint32_t *as4, bool *ipv6)
{
    size_t pos = 0;

    while (pos < len) {
        if (len - pos < 2 || len - pos - 2 < p[pos + 1]) {
            return err(ERR_OPEN, ERR_OPEN_UNSPECIFIC);
        }
        uint8_t code = p[pos];
        uint8_t clen = p[pos + 1];
        const uint8_t *v = &p[pos + 2];
        if (code == CAP_MULTIPROTOCOL || code == CAP_AS4) {
            if (clen != 4) {
                return err(ERR_OPEN, ERR_OPEN_UNSPECIFIC);
            }
            if (code == CAP_AS4) {
                *has_as4 = true;
                *as4 = get32(v);
            } else if (get16(v) == AFI_IPV6 && v[3] == SAFI_UNICAST) {
                *ipv6 = true;
            }
        }
        pos += 2 + (size_t)clen;
    }
    return OK;
}

routing_bgp_err_t routing_bgp_decode_open(const uint8_t *msg, size_t len, uint32_t expected_asn,
                                          routing_bgp_open_t *out)
{
    if (len < OPEN_MIN_LEN) {
        return err(ERR_HEADER, ERR_HEADER_LENGTH);
    }
    const uint8_t *p = &msg[ROUTING_BGP_HDR_LEN];
    if (p[0] != 4) {
        return err(ERR_OPEN, ERR_OPEN_VERSION);
    }
    uint16_t my_as = get16(&p[1]);
    uint16_t hold = get16(&p[3]);
    uint32_t bgp_id = get32(&p[5]);
    size_t params_len = p[9];
    if (OPEN_MIN_LEN + params_len != len) {
        return err(ERR_OPEN, ERR_OPEN_UNSPECIFIC);
    }

    bool has_as4 = false;
    uint32_t as4 = 0;
    bool ipv6 = false;
    const uint8_t *params = &msg[OPEN_MIN_LEN];
    size_t pos = 0;
    while (pos < params_len) {
        if (params_len - pos < 2 || params_len - pos - 2 < params[pos + 1]) {
            return err(ERR_OPEN, ERR_OPEN_UNSPECIFIC);
        }
        if (params[pos] != OPT_PARAM_CAPABILITIES) {
            return err(ERR_OPEN, ERR_OPEN_OPT_PARAM);
        }
        routing_bgp_err_t e = parse_caps(&params[pos + 2], params[pos + 1], &has_as4, &as4, &ipv6);
        if (e.code != 0) {
            return e;
        }
        pos += 2 + (size_t)params[pos + 1];
    }

    uint32_t asn = has_as4 ? as4 : my_as;
    if (asn != expected_asn) {
        return err(ERR_OPEN, ERR_OPEN_PEER_AS);
    }
    if (bgp_id == 0) {
        return err(ERR_OPEN, ERR_OPEN_BGP_ID);
    }
    if (hold == 1 || hold == 2) {
        return err(ERR_OPEN, ERR_OPEN_HOLD_TIME);
    }
    if (!ipv6) {
        return err(ERR_OPEN, ERR_OPEN_CAPABILITY);
    }
    out->asn = asn;
    out->router_id = bgp_id;
    out->hold_time = hold;
    out->ipv6_unicast = ipv6;
    return OK;
}

/* Counts the prefixes in [p, p + len), each a length byte and ceil(length / 8) bytes. */
static routing_bgp_err_t count_prefixes(const uint8_t *p, size_t len, uint8_t max_bits, uint32_t *count)
{
    size_t pos = 0;

    while (pos < len) {
        uint8_t bits = p[pos];
        size_t bytes = ((size_t)bits + 7) / 8;
        if (bits > max_bits || len - pos - 1 < bytes) {
            return err(ERR_UPDATE, ERR_UPDATE_NETWORK);
        }
        (*count)++;
        pos += 1 + bytes;
    }
    return OK;
}

/* MP_REACH_NLRI value: AFI, SAFI, next-hop length, next-hop, reserved, NLRI. Only IPv6 unicast is counted. */
static routing_bgp_err_t count_mp_reach(const uint8_t *v, size_t len, uint32_t *count)
{
    if (len < 5 || len - 5 < v[3]) {
        return err(ERR_UPDATE, ERR_UPDATE_OPT_ATTR);
    }
    if (get16(v) != AFI_IPV6 || v[2] != SAFI_UNICAST) {
        return OK;
    }
    size_t nlri = 5 + (size_t)v[3];
    return count_prefixes(&v[nlri], len - nlri, 128, count);
}

routing_bgp_err_t routing_bgp_count_update_nlri(const uint8_t *msg, size_t len, uint32_t *count)
{
    if (len < UPDATE_MIN_LEN) {
        return err(ERR_HEADER, ERR_HEADER_LENGTH);
    }
    size_t pos = ROUTING_BGP_HDR_LEN;
    size_t withdrawn_len = get16(&msg[pos]);
    pos += 2;
    if (len - pos - 2 < withdrawn_len) {
        return err(ERR_UPDATE, ERR_UPDATE_ATTR_LIST);
    }
    pos += withdrawn_len;
    size_t attrs_len = get16(&msg[pos]);
    pos += 2;
    if (len - pos < attrs_len) {
        return err(ERR_UPDATE, ERR_UPDATE_ATTR_LIST);
    }

    uint32_t n = 0;
    const uint8_t *attrs = &msg[pos];
    size_t a = 0;
    while (a < attrs_len) {
        if (attrs_len - a < 3) {
            return err(ERR_UPDATE, ERR_UPDATE_ATTR_LIST);
        }
        uint8_t flags = attrs[a];
        uint8_t type = attrs[a + 1];
        size_t value_len;
        size_t hdr;
        if (flags & ATTR_FLAG_EXT_LEN) {
            if (attrs_len - a < 4) {
                return err(ERR_UPDATE, ERR_UPDATE_ATTR_LIST);
            }
            value_len = get16(&attrs[a + 2]);
            hdr = 4;
        } else {
            value_len = attrs[a + 2];
            hdr = 3;
        }
        if (attrs_len - a - hdr < value_len) {
            return err(ERR_UPDATE, ERR_UPDATE_ATTR_LIST);
        }
        if (type == ATTR_MP_REACH) {
            routing_bgp_err_t e = count_mp_reach(&attrs[a + hdr], value_len, &n);
            if (e.code != 0) {
                return e;
            }
        }
        a += hdr + value_len;
    }
    pos += attrs_len;

    routing_bgp_err_t e = count_prefixes(&msg[pos], len - pos, 32, &n);
    if (e.code != 0) {
        return e;
    }
    *count = n;
    return OK;
}

routing_bgp_err_t routing_bgp_decode_notification(const uint8_t *msg, size_t len, uint8_t *code, uint8_t *subcode)
{
    if (len < NOTIFICATION_MIN_LEN) {
        return err(ERR_HEADER, ERR_HEADER_LENGTH);
    }
    *code = msg[ROUTING_BGP_HDR_LEN];
    *subcode = msg[ROUTING_BGP_HDR_LEN + 1];
    return OK;
}
