#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "hosted_frame.h"
#include "hosted_rpc.h"
#include "hosted_session.h"

static const char *TAG = "c6_hosted";

typedef struct {
    const hosted_link_t *link;
    uint8_t rx[HOSTED_RX_BUF];
    size_t rx_len;
    size_t rx_pos;
    uint8_t tx[HOSTED_MAX_PACKET];
    uint8_t proto[HOSTED_RPC_REQ_MAX];
    uint8_t serial[HOSTED_RPC_REQ_MAX + HOSTED_SERIAL_OVERHEAD];
    uint8_t chunk[HOSTED_RPC_OTA_CHUNK];
    uint16_t seq;
    uint32_t uid;
} session_t;

static void set_text(char *dst, size_t cap, const char *text)
{
    if (dst && cap) {
        strlcpy(dst, text, cap);
    }
}

/* Milliseconds left until deadline; 0 once it passed (wrap-safe). */
static uint32_t left_ms(const session_t *s, uint32_t deadline)
{
    const int32_t left = (int32_t)(deadline - s->link->now_ms());
    return left > 0 ? (uint32_t)left : 0;
}

static bool all_zero(const uint8_t *p, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (p[i]) {
            return false;
        }
    }
    return true;
}

/*
 * The next packet from the C6 before the deadline. A receive may carry several packets followed by block padding, and may
 * end inside a packet (the link returns what fits; the rest comes with the next receive): a partial packet is kept and
 * completed by the next receive. Padding or a malformed header ends that receive. ESP_ERR_TIMEOUT when nothing came in time.
 */
static esp_err_t next_packet(session_t *s, uint32_t deadline, hosted_packet_t *out)
{
    for (;;) {
        if (s->rx_pos < s->rx_len) {
            const int n = hosted_frame_next(s->rx + s->rx_pos, s->rx_len - s->rx_pos, out);
            if (n > 0) {
                s->rx_pos += (size_t)n;
                return ESP_OK;
            }
            if (n == 0 && all_zero(s->rx + s->rx_pos, s->rx_len - s->rx_pos)) {
                s->rx_pos = s->rx_len = 0; // a short padding tail, not the start of a packet
            } else if (n == 0 && s->rx_pos > 0) {
                // A partial packet: move it to the front, the next receive appends the rest.
                memmove(s->rx, s->rx + s->rx_pos, s->rx_len - s->rx_pos);
                s->rx_len -= s->rx_pos;
                s->rx_pos = 0;
            } else if (n < 0 || s->rx_len == sizeof(s->rx)) {
                s->rx_pos = s->rx_len = 0; // padding, a malformed header, or a packet that cannot fit: drop
            }
        } else {
            s->rx_pos = s->rx_len = 0;
        }
        const uint32_t left = left_ms(s, deadline);
        if (left == 0) {
            return ESP_ERR_TIMEOUT;
        }
        size_t got = 0;
        const esp_err_t err = s->link->recv(s->rx + s->rx_len, sizeof(s->rx) - s->rx_len, &got, left);
        if (err == ESP_ERR_TIMEOUT) {
            return ESP_ERR_TIMEOUT;
        } else if (err != ESP_OK) {
            return err;
        }
        s->rx_len += got;
    }
}

/* Open the data path and wait for the PRIV startup event. *present: it came from an ESP32-C6. */
static esp_err_t await_startup(session_t *s, bool *present)
{
    *present = false;
    esp_err_t err = s->link->open_data_path();
    if (err != ESP_OK) {
        return err;
    }
    const uint32_t deadline = s->link->now_ms() + HOSTED_DETECT_TIMEOUT_MS;
    hosted_packet_t pkt;
    while ((err = next_packet(s, deadline, &pkt)) == ESP_OK) {
        uint8_t chip = 0;
        if (hosted_priv_init_chip_id(&pkt, &chip) == ESP_OK) {
            *present = chip == HOSTED_CHIP_ID_ESP32C6;
            if (!*present) {
                ESP_LOGW(TAG, "ESP-Hosted answered with chip ID 0x%02x, not an ESP32-C6", chip);
            }
            return ESP_OK;
        }
    }
    return err == ESP_ERR_TIMEOUT ? ESP_OK : err;
}

/* Start the link; ESP_ERR_NOT_FOUND from it means no ESP-Hosted, not a failure. */
static esp_err_t start(session_t *s, bool *present)
{
    *present = false;
    const esp_err_t err = s->link->start();
    if (err == ESP_ERR_NOT_FOUND) {
        return ESP_OK;
    }
    return err == ESP_OK ? await_startup(s, present) : err;
}

esp_err_t hosted_session_detect(const hosted_link_t *link, bool *present)
{
    if (!link || !present) {
        return ESP_ERR_INVALID_ARG;
    }
    *present = false;
    session_t *s = calloc(1, sizeof(*s));
    if (!s) {
        return ESP_ERR_NO_MEM;
    }
    s->link = link;
    const esp_err_t err = start(s, present);
    link->release();
    free(s);
    return err;
}

static const char *step_name(uint32_t msg_id)
{
    switch (msg_id) {
    case HOSTED_RPC_REQ_OTA_BEGIN:
        return "OTABegin";
    case HOSTED_RPC_REQ_OTA_WRITE:
        return "OTAWrite";
    default:
        return "OTAEnd";
    }
}

/* One RPC: send the request, wait for its response (events and other packets are skipped). */
static esp_err_t request(session_t *s, uint32_t msg_id, const uint8_t *data, size_t len, uint32_t timeout_ms, char *detail,
                         size_t cap)
{
    char text[64];
    const size_t proto_len = hosted_rpc_encode_req(msg_id, ++s->uid, data, len, s->proto, sizeof(s->proto));
    const size_t serial_len = proto_len ? hosted_serial_wrap(s->proto, proto_len, s->serial, sizeof(s->serial)) : 0;
    const size_t pkt_len = serial_len ? hosted_frame_encode(HOSTED_IF_SERIAL, s->seq++, s->serial, (uint16_t)serial_len,
                                                            s->tx, sizeof(s->tx)) : 0;
    if (!pkt_len) {
        snprintf(text, sizeof(text), "%s does not fit a packet", step_name(msg_id));
        set_text(detail, cap, text);
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t err = s->link->send(s->tx, pkt_len, HOSTED_WRITE_TIMEOUT_MS);
    if (err != ESP_OK) {
        snprintf(text, sizeof(text), "sending %s failed", step_name(msg_id));
        set_text(detail, cap, text);
        return err;
    }

    const uint32_t deadline = s->link->now_ms() + timeout_ms;
    hosted_packet_t pkt;
    while ((err = next_packet(s, deadline, &pkt)) == ESP_OK) {
        bool is_event = false;
        const uint8_t *proto = NULL;
        size_t n = 0;
        hosted_rpc_msg_t msg;
        if (pkt.if_type != HOSTED_IF_SERIAL || hosted_serial_unwrap(pkt.payload, pkt.len, &is_event, &proto, &n) != ESP_OK ||
                is_event || hosted_rpc_decode(proto, n, &msg) != ESP_OK || msg.msg_type != HOSTED_RPC_TYPE_RESP ||
                msg.msg_id != msg_id + HOSTED_RPC_RESP_OFFSET) {
            continue;
        }
        if (msg.resp != 0) {
            snprintf(text, sizeof(text), "%s refused (%d)", step_name(msg_id), (int)msg.resp);
            set_text(detail, cap, text);
            return ESP_FAIL;
        }
        return ESP_OK;
    }
    snprintf(text, sizeof(text), "no answer to %s", step_name(msg_id));
    set_text(detail, cap, text);
    return err;
}

static esp_err_t ota(session_t *s, const c6_hosted_source_t *src, char *detail, size_t cap)
{
    bool present = false;
    esp_err_t err = start(s, &present);
    if (err != ESP_OK) {
        set_text(detail, cap, "SDIO bus failed");
        return err;
    }
    if (!present) {
        set_text(detail, cap, "no ESP-Hosted answer");
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "ESP-Hosted OTA of %u bytes", (unsigned)src->total_size);
    err = request(s, HOSTED_RPC_REQ_OTA_BEGIN, NULL, 0, HOSTED_BEGIN_TIMEOUT_MS, detail, cap);
    for (size_t off = 0; err == ESP_OK && off < src->total_size; off += sizeof(s->chunk)) {
        const size_t n = src->total_size - off < sizeof(s->chunk) ? src->total_size - off : sizeof(s->chunk);
        err = src->read(src->ctx, off, s->chunk, n);
        if (err != ESP_OK) {
            set_text(detail, cap, "reading the image failed");
            return err;
        }
        err = request(s, HOSTED_RPC_REQ_OTA_WRITE, s->chunk, n, HOSTED_WRITE_TIMEOUT_MS, detail, cap);
    }
    if (err == ESP_OK) {
        err = request(s, HOSTED_RPC_REQ_OTA_END, NULL, 0, HOSTED_END_TIMEOUT_MS, detail, cap);
    }
    return err;
}

esp_err_t hosted_session_ota(const hosted_link_t *link, const c6_hosted_source_t *source, char *err_detail, size_t err_cap)
{
    set_text(err_detail, err_cap, "");
    if (!link || !source || !source->read || source->total_size == 0) {
        set_text(err_detail, err_cap, "no image");
        return ESP_ERR_INVALID_ARG;
    }
    session_t *s = calloc(1, sizeof(*s));
    if (!s) {
        set_text(err_detail, err_cap, "out of memory");
        return ESP_ERR_NO_MEM;
    }
    s->link = link;
    const esp_err_t err = ota(s, source, err_detail, err_cap);
    link->release();
    free(s);
    return err;
}
