#include <string.h>
#include "hosted_rpc.h"

#define WT_VARINT 0
#define WT_I64    1
#define WT_LEN    2
#define WT_I32    5

typedef struct {
    uint8_t *buf;
    size_t cap;
    size_t len;
    bool overflow;
} writer_t;

static void put_byte(writer_t *w, uint8_t b)
{
    if (w->len < w->cap) {
        w->buf[w->len++] = b;
    } else {
        w->overflow = true;
    }
}

static void put_varint(writer_t *w, uint64_t v)
{
    do {
        uint8_t b = v & 0x7F;
        v >>= 7;
        put_byte(w, (uint8_t)(b | (v ? 0x80 : 0)));
    } while (v);
}

static void put_tag(writer_t *w, uint32_t field, uint32_t wire_type)
{
    put_varint(w, ((uint64_t)field << 3) | wire_type);
}

static size_t varint_len(uint64_t v)
{
    size_t n = 1;
    while (v >= 0x80) {
        v >>= 7;
        n++;
    }
    return n;
}

size_t hosted_rpc_encode_req(uint32_t msg_id, uint32_t uid, const uint8_t *data, size_t len, uint8_t *out, size_t cap)
{
    const bool is_write = msg_id == HOSTED_RPC_REQ_OTA_WRITE;

    if (!out || (msg_id != HOSTED_RPC_REQ_OTA_BEGIN && !is_write && msg_id != HOSTED_RPC_REQ_OTA_END) ||
            (is_write ? (!data && len) : (data || len))) {
        return 0;
    }
    writer_t w = {.buf = out, .cap = cap};

    put_tag(&w, 1, WT_VARINT);
    put_varint(&w, HOSTED_RPC_TYPE_REQ);
    put_tag(&w, 2, WT_VARINT);
    put_varint(&w, msg_id);
    put_tag(&w, 3, WT_VARINT);
    put_varint(&w, uid);
    // The oneof member is always present, even when empty: the C6 dispatches on msg_id and unpacks the member.
    put_tag(&w, msg_id, WT_LEN);
    if (is_write) {
        const size_t inner = 1 + varint_len(len) + len; // field 1, bytes
        put_varint(&w, inner);
        put_tag(&w, 1, WT_LEN);
        put_varint(&w, len);
        for (size_t i = 0; i < len; i++) {
            put_byte(&w, data[i]);
        }
    } else {
        put_varint(&w, 0);
    }
    return w.overflow ? 0 : w.len;
}

typedef struct {
    const uint8_t *p;
    size_t len;
    size_t pos;
} reader_t;

static bool get_varint(reader_t *r, uint64_t *v)
{
    uint64_t out = 0;

    for (int shift = 0; shift < 64; shift += 7) {
        if (r->pos >= r->len) {
            return false;
        }
        const uint8_t b = r->p[r->pos++];
        out |= (uint64_t)(b & 0x7F) << shift;
        if (!(b & 0x80)) {
            *v = out;
            return true;
        }
    }
    return false;
}

/* Skip the value of a field with this wire type. */
static bool skip(reader_t *r, uint32_t wire_type)
{
    uint64_t v;

    switch (wire_type) {
    case WT_VARINT:
        return get_varint(r, &v);
    case WT_I64:
        return (r->pos += 8) <= r->len;
    case WT_I32:
        return (r->pos += 4) <= r->len;
    case WT_LEN:
        return get_varint(r, &v) && v <= r->len - r->pos && (r->pos += (size_t)v) <= r->len;
    default:
        return false;
    }
}

/* Resp_* { int32 resp = 1; }: negative values come as 10-byte varints. */
static bool decode_resp(const uint8_t *p, size_t len, int32_t *resp)
{
    reader_t r = {.p = p, .len = len};

    while (r.pos < r.len) {
        uint64_t key, v;
        if (!get_varint(&r, &key)) {
            return false;
        }
        if ((key >> 3) == 1 && (key & 7) == WT_VARINT) {
            if (!get_varint(&r, &v)) {
                return false;
            }
            *resp = (int32_t)(uint32_t)v;
        } else if (!skip(&r, (uint32_t)(key & 7))) {
            return false;
        }
    }
    return true;
}

esp_err_t hosted_rpc_decode(const uint8_t *buf, size_t len, hosted_rpc_msg_t *out)
{
    if (!buf || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    reader_t r = {.p = buf, .len = len};
    hosted_rpc_msg_t m = {0};

    while (r.pos < r.len) {
        uint64_t key, v;
        if (!get_varint(&r, &key)) {
            return ESP_ERR_INVALID_RESPONSE;
        }
        const uint32_t field = (uint32_t)(key >> 3);
        const uint32_t wt = (uint32_t)(key & 7);
        if (field >= 1 && field <= 3 && wt == WT_VARINT) {
            if (!get_varint(&r, &v)) {
                return ESP_ERR_INVALID_RESPONSE;
            }
            if (field == 1) {
                m.msg_type = (uint32_t)v;
            } else if (field == 2) {
                m.msg_id = (uint32_t)v;
            } else {
                m.uid = (uint32_t)v;
            }
        } else if (wt == WT_LEN && field >= HOSTED_RPC_REQ_OTA_BEGIN + HOSTED_RPC_RESP_OFFSET &&
                   field <= HOSTED_RPC_REQ_OTA_END + HOSTED_RPC_RESP_OFFSET) {
            if (!get_varint(&r, &v) || v > r.len - r.pos || !decode_resp(r.p + r.pos, (size_t)v, &m.resp)) {
                return ESP_ERR_INVALID_RESPONSE;
            }
            r.pos += (size_t)v;
        } else if (!skip(&r, wt)) {
            return ESP_ERR_INVALID_RESPONSE;
        }
    }
    *out = m;
    return ESP_OK;
}
