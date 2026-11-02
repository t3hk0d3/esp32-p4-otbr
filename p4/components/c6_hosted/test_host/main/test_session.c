#include <stdlib.h>
#include <string.h>
#include "unity.h"
#include "unity_test_runner.h"
#include "hosted_frame.h"
#include "hosted_rpc.h"
#include "hosted_session.h"

/* ---- scripted ESP-Hosted slave ---- */

#define IMAGE_SIZE 5000 /* four OTAWrite chunks */
#define RXQ_MAX    64
#define LOG_MAX    64

typedef struct {
    /* script */
    esp_err_t start_err;
    bool esp_hosted;        /* sends the PRIV init event once the data path opens */
    uint8_t chip_id;
    int32_t resp[3];        /* resp per BEGIN / WRITE / END */
    int silent_msg;         /* this request id never gets an answer (0: all answered) */
    bool event_before_resp; /* an RPC event goes out right before every response */
    int fail_read_at;       /* source read at this offset fails (-1: never) */
    bool short_pad;         /* receives end in 4 zero bytes instead of padding to a block */
    /* state */
    uint32_t now;
    uint8_t rxq[RXQ_MAX][HOSTED_RX_BUF];
    size_t rxq_len[RXQ_MAX];
    int rxq_head, rxq_tail;
    /* observations */
    int start_count, release_count;
    uint32_t log[LOG_MAX]; /* request ids in order */
    uint32_t uids[LOG_MAX];
    int log_n;
    uint8_t written[IMAGE_SIZE + 2000];
    size_t written_len;
    size_t max_chunk;
} slave_t;

static slave_t sl;
static uint8_t image[IMAGE_SIZE];

static void setup(void)
{
    memset(&sl, 0, sizeof(sl));
    sl.esp_hosted = true;
    sl.chip_id = HOSTED_CHIP_ID_ESP32C6;
    sl.fail_read_at = -1;
    for (int i = 0; i < IMAGE_SIZE; i++) {
        image[i] = (uint8_t)(i * 13 + 1);
    }
}

/* Queue one receive: the packet, followed by zero block padding. */
static void queue_packet(uint8_t if_type, uint8_t pkt_type, const uint8_t *payload, uint16_t len)
{
    TEST_ASSERT_TRUE(sl.rxq_tail < RXQ_MAX);
    uint8_t *buf = sl.rxq[sl.rxq_tail];
    memset(buf, 0, 512);
    const size_t n = hosted_frame_encode(if_type, 0, payload, len, buf, HOSTED_RX_BUF);
    TEST_ASSERT_TRUE(n > 0);
    buf[11] = pkt_type;
    sl.rxq_len[sl.rxq_tail++] = sl.short_pad ? n + 4 : (n < 512 ? 512 : n);
}

/* Split the receives so that a packet straddles two of them (the link returned only what fit). */
static bool s_split_next_resp;

static void split_last_queued(void)
{
    /* Move the last queued receive's second half into a new receive after it. */
    TEST_ASSERT_TRUE(sl.rxq_tail >= 1 && sl.rxq_tail < RXQ_MAX);
    const int last = sl.rxq_tail - 1;
    const size_t n = sl.rxq_len[last];
    const size_t first = 20; /* inside the packet: header plus a few payload bytes */
    memcpy(sl.rxq[sl.rxq_tail], sl.rxq[last] + first, n - first);
    sl.rxq_len[sl.rxq_tail++] = n - first;
    sl.rxq_len[last] = first;
}

static size_t put_varint(uint8_t *p, uint64_t v)
{
    size_t n = 0;
    do {
        uint8_t b = v & 0x7F;
        v >>= 7;
        p[n++] = (uint8_t)(b | (v ? 0x80 : 0));
    } while (v);
    return n;
}

static void queue_rpc(uint32_t msg_type, uint32_t msg_id, uint32_t uid, int32_t resp)
{
    uint8_t inner[16], proto[64], serial[96];
    size_t ni = 0, np = 0;
    inner[ni++] = 0x08;
    ni += put_varint(inner + ni, (uint64_t)(int64_t)resp);
    np += put_varint(proto + np, (1 << 3) | 0);
    np += put_varint(proto + np, msg_type);
    np += put_varint(proto + np, (2 << 3) | 0);
    np += put_varint(proto + np, msg_id);
    np += put_varint(proto + np, (3 << 3) | 0);
    np += put_varint(proto + np, uid);
    np += put_varint(proto + np, ((uint64_t)msg_id << 3) | 2);
    np += put_varint(proto + np, ni);
    memcpy(proto + np, inner, ni);
    np += ni;
    size_t ns = hosted_serial_wrap(proto, np, serial, sizeof(serial));
    if (msg_type == HOSTED_RPC_TYPE_EVENT) {
        memcpy(serial + 3, "RPCEvt", 6);
    }
    queue_packet(HOSTED_IF_SERIAL, 0, serial, (uint16_t)ns);
}

static esp_err_t f_start(void)
{
    sl.start_count++;
    return sl.start_err;
}

static esp_err_t f_open(void)
{
    if (sl.esp_hosted) {
        const uint8_t ev[] = {HOSTED_PRIV_EVENT_INIT, 6, 0x11, 1, 0x3F, HOSTED_PRIV_TAG_CHIP_ID, 1, sl.chip_id};
        queue_packet(HOSTED_IF_PRIV, HOSTED_PRIV_PKT_EVENT, ev, sizeof(ev));
        queue_rpc(HOSTED_RPC_TYPE_EVENT, 769, 0, 0); /* ESPInit */
    }
    return ESP_OK;
}

static uint64_t get_varint(const uint8_t **p)
{
    uint64_t v = 0;
    for (int shift = 0;; shift += 7) {
        const uint8_t b = *(*p)++;
        v |= (uint64_t)(b & 0x7F) << shift;
        if (!(b & 0x80)) {
            return v;
        }
    }
}

static esp_err_t f_send(const uint8_t *buf, size_t len, uint32_t timeout_ms)
{
    (void)timeout_ms;
    hosted_packet_t pkt;
    TEST_ASSERT_EQUAL((int)len, hosted_frame_next(buf, len, &pkt));
    TEST_ASSERT_EQUAL(HOSTED_IF_SERIAL, pkt.if_type);
    bool is_event = true;
    const uint8_t *proto = NULL;
    size_t n = 0;
    TEST_ASSERT_EQUAL(ESP_OK, hosted_serial_unwrap(pkt.payload, pkt.len, &is_event, &proto, &n));
    hosted_rpc_msg_t m;
    TEST_ASSERT_EQUAL(ESP_OK, hosted_rpc_decode(proto, n, &m));
    TEST_ASSERT_EQUAL(HOSTED_RPC_TYPE_REQ, m.msg_type);
    TEST_ASSERT_TRUE(sl.log_n < LOG_MAX);
    sl.uids[sl.log_n] = m.uid;
    sl.log[sl.log_n++] = m.msg_id;

    if (m.msg_id == HOSTED_RPC_REQ_OTA_WRITE) {
        /* Rpc fields 1..3 are three tag/varint pairs; then field 273 LEN { field 1 LEN data }. */
        const uint8_t *p = proto;
        for (int i = 0; i < 3; i++) {
            get_varint(&p);
            get_varint(&p);
        }
        TEST_ASSERT_EQUAL((HOSTED_RPC_REQ_OTA_WRITE << 3) | 2, get_varint(&p));
        get_varint(&p);
        TEST_ASSERT_EQUAL((1 << 3) | 2, get_varint(&p));
        const size_t dlen = (size_t)get_varint(&p);
        TEST_ASSERT_TRUE(sl.written_len + dlen <= sizeof(sl.written));
        memcpy(sl.written + sl.written_len, p, dlen);
        sl.written_len += dlen;
        if (dlen > sl.max_chunk) {
            sl.max_chunk = dlen;
        }
    }
    if ((int)m.msg_id != sl.silent_msg) {
        if (sl.event_before_resp) {
            queue_rpc(HOSTED_RPC_TYPE_EVENT, 769, 0, 0);
        }
        queue_rpc(HOSTED_RPC_TYPE_RESP, m.msg_id + HOSTED_RPC_RESP_OFFSET, m.uid,
                  sl.resp[m.msg_id - HOSTED_RPC_REQ_OTA_BEGIN]);
        if (s_split_next_resp) {
            split_last_queued();
        }
    }
    return ESP_OK;
}

static esp_err_t f_recv(uint8_t *buf, size_t cap, size_t *len, uint32_t timeout_ms)
{
    if (sl.rxq_head == sl.rxq_tail) {
        sl.now += timeout_ms;
        return ESP_ERR_TIMEOUT;
    }
    TEST_ASSERT_TRUE(sl.rxq_len[sl.rxq_head] <= cap);
    memcpy(buf, sl.rxq[sl.rxq_head], sl.rxq_len[sl.rxq_head]);
    *len = sl.rxq_len[sl.rxq_head++];
    sl.now += 1;
    return ESP_OK;
}

static void f_release(void)
{
    sl.release_count++;
}

static uint32_t f_now(void)
{
    return sl.now;
}

static const hosted_link_t s_link = {
    .start = f_start, .open_data_path = f_open, .send = f_send, .recv = f_recv, .release = f_release, .now_ms = f_now,
};

static esp_err_t src_read(void *ctx, size_t offset, void *buf, size_t len)
{
    (void)ctx;
    if (sl.fail_read_at >= 0 && offset == (size_t)sl.fail_read_at) {
        return ESP_ERR_INVALID_STATE;
    }
    memcpy(buf, image + offset, len);
    return ESP_OK;
}

static const c6_hosted_source_t s_src = {.total_size = IMAGE_SIZE, .read = src_read};

static int count_msg(uint32_t id)
{
    int n = 0;
    for (int i = 0; i < sl.log_n; i++) {
        n += sl.log[i] == id;
    }
    return n;
}

/* ---- hosted_session_detect ---- */

TEST_CASE("On PRIV with chip ID ESP32-C6: present is true and the link is released", "[session]")
{
    setup();
    bool present = false;

    TEST_ASSERT_EQUAL(ESP_OK, hosted_session_detect(&s_link, &present));
    TEST_ASSERT_TRUE(present);
    TEST_ASSERT_EQUAL(1, sl.release_count);
    TEST_ASSERT_EQUAL(0, sl.log_n);
}

TEST_CASE("On silence: present is false, ESP_OK after the detect timeout, released", "[session]")
{
    setup();
    sl.esp_hosted = false;
    bool present = true;

    TEST_ASSERT_EQUAL(ESP_OK, hosted_session_detect(&s_link, &present));
    TEST_ASSERT_FALSE(present);
    TEST_ASSERT_TRUE(sl.now >= HOSTED_DETECT_TIMEOUT_MS);
    TEST_ASSERT_EQUAL(1, sl.release_count);
}

TEST_CASE("On another chip ID: present is false", "[session]")
{
    setup();
    sl.chip_id = 0x12; /* ESP32-P4 */
    bool present = true;

    TEST_ASSERT_EQUAL(ESP_OK, hosted_session_detect(&s_link, &present));
    TEST_ASSERT_FALSE(present);
}

TEST_CASE("On no SDIO slave at card init: present is false, ESP_OK, released", "[session]")
{
    setup();
    sl.start_err = ESP_ERR_NOT_FOUND;
    bool present = true;

    TEST_ASSERT_EQUAL(ESP_OK, hosted_session_detect(&s_link, &present));
    TEST_ASSERT_FALSE(present);
    TEST_ASSERT_EQUAL(1, sl.release_count);
}

TEST_CASE("On the bus failing to start: the error, present false, released", "[session]")
{
    setup();
    sl.start_err = ESP_FAIL;
    bool present = true;

    TEST_ASSERT_EQUAL(ESP_FAIL, hosted_session_detect(&s_link, &present));
    TEST_ASSERT_FALSE(present);
    TEST_ASSERT_EQUAL(1, sl.release_count);
}

/* ---- hosted_session_ota ---- */

TEST_CASE("On a slave answering everything: BEGIN, WRITEs carrying the source in order, END", "[session]")
{
    setup();
    char detail[64];

    TEST_ASSERT_EQUAL(ESP_OK, hosted_session_ota(&s_link, &s_src, detail, sizeof(detail)));
    TEST_ASSERT_EQUAL_STRING("", detail);
    TEST_ASSERT_EQUAL(HOSTED_RPC_REQ_OTA_BEGIN, sl.log[0]);
    TEST_ASSERT_EQUAL(HOSTED_RPC_REQ_OTA_END, sl.log[sl.log_n - 1]);
    TEST_ASSERT_EQUAL(sl.log_n - 2, count_msg(HOSTED_RPC_REQ_OTA_WRITE));
    TEST_ASSERT_EQUAL(IMAGE_SIZE, sl.written_len);
    TEST_ASSERT_EQUAL_MEMORY(image, sl.written, IMAGE_SIZE);
    TEST_ASSERT_TRUE(sl.max_chunk <= HOSTED_RPC_OTA_CHUNK);
    TEST_ASSERT_EQUAL(1, sl.release_count);
}

TEST_CASE("On a slave answering everything: each request carries a new uid", "[session]")
{
    setup();
    TEST_ASSERT_EQUAL(ESP_OK, hosted_session_ota(&s_link, &s_src, NULL, 0));
    for (int i = 1; i < sl.log_n; i++) {
        TEST_ASSERT_TRUE(sl.uids[i] > sl.uids[i - 1]);
    }
}

TEST_CASE("On events before every response: they are skipped", "[session]")
{
    setup();
    sl.event_before_resp = true;
    TEST_ASSERT_EQUAL(ESP_OK, hosted_session_ota(&s_link, &s_src, NULL, 0));
    TEST_ASSERT_EQUAL_MEMORY(image, sl.written, IMAGE_SIZE);
}

TEST_CASE("On responses split across two receives: they are reassembled", "[session]")
{
    setup();
    s_split_next_resp = true;
    const esp_err_t err = hosted_session_ota(&s_link, &s_src, NULL, 0);
    s_split_next_resp = false;
    TEST_ASSERT_EQUAL(ESP_OK, err);
    TEST_ASSERT_EQUAL_MEMORY(image, sl.written, IMAGE_SIZE);
}

TEST_CASE("On a receive ending in a short zero padding tail: the next packet is still read", "[session]")
{
    setup();
    sl.short_pad = true;
    TEST_ASSERT_EQUAL(ESP_OK, hosted_session_ota(&s_link, &s_src, NULL, 0));
    TEST_ASSERT_EQUAL_MEMORY(image, sl.written, IMAGE_SIZE);
}

TEST_CASE("On OTABegin refused: ESP_FAIL naming the step, nothing written", "[session]")
{
    setup();
    sl.resp[0] = -1;
    char detail[64];

    TEST_ASSERT_EQUAL(ESP_FAIL, hosted_session_ota(&s_link, &s_src, detail, sizeof(detail)));
    TEST_ASSERT_EQUAL_STRING("OTABegin refused (-1)", detail);
    TEST_ASSERT_EQUAL(0, count_msg(HOSTED_RPC_REQ_OTA_WRITE));
    TEST_ASSERT_EQUAL(1, sl.release_count);
}

TEST_CASE("On OTAWrite refused: ESP_FAIL naming the step, no OTAEnd", "[session]")
{
    setup();
    sl.resp[1] = -1;
    char detail[64];

    TEST_ASSERT_EQUAL(ESP_FAIL, hosted_session_ota(&s_link, &s_src, detail, sizeof(detail)));
    TEST_ASSERT_EQUAL_STRING("OTAWrite refused (-1)", detail);
    TEST_ASSERT_EQUAL(1, count_msg(HOSTED_RPC_REQ_OTA_WRITE));
    TEST_ASSERT_EQUAL(0, count_msg(HOSTED_RPC_REQ_OTA_END));
}

TEST_CASE("On OTAEnd refused: ESP_FAIL naming the step", "[session]")
{
    setup();
    sl.resp[2] = 5;
    char detail[64];

    TEST_ASSERT_EQUAL(ESP_FAIL, hosted_session_ota(&s_link, &s_src, detail, sizeof(detail)));
    TEST_ASSERT_EQUAL_STRING("OTAEnd refused (5)", detail);
}

TEST_CASE("On no answer to OTABegin: ESP_ERR_TIMEOUT naming the step after the begin timeout, released", "[session]")
{
    setup();
    sl.silent_msg = HOSTED_RPC_REQ_OTA_BEGIN;
    char detail[64];

    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, hosted_session_ota(&s_link, &s_src, detail, sizeof(detail)));
    TEST_ASSERT_EQUAL_STRING("no answer to OTABegin", detail);
    TEST_ASSERT_TRUE(sl.now >= HOSTED_BEGIN_TIMEOUT_MS);
    TEST_ASSERT_EQUAL(0, count_msg(HOSTED_RPC_REQ_OTA_WRITE));
    TEST_ASSERT_EQUAL(1, sl.release_count);
}

TEST_CASE("On no answer to OTAWrite: ESP_ERR_TIMEOUT, no OTAEnd", "[session]")
{
    setup();
    sl.silent_msg = HOSTED_RPC_REQ_OTA_WRITE;
    char detail[64];

    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, hosted_session_ota(&s_link, &s_src, detail, sizeof(detail)));
    TEST_ASSERT_EQUAL_STRING("no answer to OTAWrite", detail);
    TEST_ASSERT_EQUAL(0, count_msg(HOSTED_RPC_REQ_OTA_END));
}

TEST_CASE("On a source read error: its error, no OTAEnd, released", "[session]")
{
    setup();
    sl.fail_read_at = 2 * HOSTED_RPC_OTA_CHUNK;
    char detail[64];

    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, hosted_session_ota(&s_link, &s_src, detail, sizeof(detail)));
    TEST_ASSERT_EQUAL_STRING("reading the image failed", detail);
    TEST_ASSERT_EQUAL(2, count_msg(HOSTED_RPC_REQ_OTA_WRITE));
    TEST_ASSERT_EQUAL(0, count_msg(HOSTED_RPC_REQ_OTA_END));
    TEST_ASSERT_EQUAL(1, sl.release_count);
}

TEST_CASE("On no ESP-Hosted answer: ESP_ERR_NOT_FOUND, no request sent, released", "[session]")
{
    setup();
    sl.esp_hosted = false;
    char detail[64];

    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, hosted_session_ota(&s_link, &s_src, detail, sizeof(detail)));
    TEST_ASSERT_EQUAL_STRING("no ESP-Hosted answer", detail);
    TEST_ASSERT_EQUAL(0, sl.log_n);
    TEST_ASSERT_EQUAL(1, sl.release_count);
}

void app_main(void)
{
    UNITY_BEGIN();
    unity_run_all_tests();
    int failures = (int)UNITY_END();
    exit(failures == 0 ? 0 : 1);
}
