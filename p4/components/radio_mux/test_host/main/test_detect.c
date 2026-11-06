#include <string.h>
#include "radio_mux_detect.h"
#include "unity.h"
#include "unity_test_runner.h"

#define T0 1000
#define KOENKK "OPENTHREAD/1.4.0-Koenkk-2025.3.1; CC13XX_CC26XX; Mar 13 2025"

typedef struct {
    radio_mux_probe_action_t act;
    uint32_t baud;
    const uint8_t *tx;
    size_t tx_n;
    int64_t until;
    radio_mux_fw_record_t rec;
    bool new_step;
} out_t;

static radio_mux_detect_t det;

static out_t call(int64_t now, const uint8_t *rx, size_t rx_n)
{
    out_t o;

    memset(&o, 0, sizeof(o));
    o.act = radio_mux_detect_next(&det, now, rx, rx_n, &o.baud, &o.tx, &o.tx_n, &o.until, &o.rec);
    o.new_step = det.new_step;
    return o;
}

static out_t begin(void)
{
    radio_mux_detect_start(&det, T0);
    return call(T0, NULL, 0);
}

// Silence until the deadline of the current step: the next call moves on.
static out_t timeout_of(const out_t *o)
{
    return call(o->until, NULL, 0);
}

// Spinel frame -> HDLC bytes. Returns the length.
static size_t spinel_wire(const uint8_t *frame, size_t n, uint8_t *out, size_t cap)
{
    return radio_mux_hdlc_encode(frame, n, out, cap);
}

// Decodes the HDLC bytes of a request into `frame`; returns the length, 0 on failure.
static size_t spinel_unwire(const uint8_t *tx, size_t n, uint8_t *frame, size_t cap)
{
    radio_mux_hdlc_dec_t dec;
    uint8_t buf[32];
    bool done = false;

    radio_mux_hdlc_dec_init(&dec, buf, sizeof(buf));
    radio_mux_hdlc_decode_feed(&dec, tx, n, &done);
    if (!done || dec.len > cap) {
        return 0;
    }
    memcpy(frame, buf, dec.len);
    return dec.len;
}

static void assert_spinel_get(const out_t *o, const uint8_t *prop, size_t prop_n)
{
    uint8_t f[16];
    size_t n = spinel_unwire(o->tx, o->tx_n, f, sizeof(f));

    TEST_ASSERT_EQUAL_UINT32(2 + prop_n, n);
    TEST_ASSERT_EQUAL_HEX8(0x81, f[0]);
    TEST_ASSERT_EQUAL_HEX8(0x02, f[1]); // PROP_VALUE_GET
    TEST_ASSERT_EQUAL_HEX8_ARRAY(prop, f + 2, prop_n);
}

static const uint8_t PROP_NCP_VERSION[] = {0x02};
static const uint8_t PROP_ZIGGURAT[] = {0xDA, 0x7A};

// A spinel answer: header 0x81, command PROP_VALUE_IS (6), packed property, value.
static size_t spinel_is_hdr(uint8_t hdr, const uint8_t *prop, size_t prop_n, const uint8_t *val, size_t val_n,
                            uint8_t *out, size_t cap)
{
    uint8_t f[200];
    size_t at = 0;

    f[at++] = hdr;
    f[at++] = 0x06;
    memcpy(f + at, prop, prop_n);
    at += prop_n;
    memcpy(f + at, val, val_n);
    at += val_n;
    return spinel_wire(f, at, out, cap);
}

static size_t spinel_is(const uint8_t *prop, size_t prop_n, const uint8_t *val, size_t val_n, uint8_t *out, size_t cap)
{
    return spinel_is_hdr(0x81, prop, prop_n, val, val_n, out, cap);
}

static size_t ncp_version_answer(const char *s, uint8_t *out, size_t cap)
{
    return spinel_is(PROP_NCP_VERSION, 1, (const uint8_t *)s, strlen(s) + 1, out, cap);
}

static size_t last_status_answer(uint8_t *out, size_t cap)
{
    const uint8_t prop[] = {0x00}, val[] = {0x0E}; // LAST_STATUS: PROP_NOT_FOUND

    return spinel_is(prop, 1, val, 1, out, cap);
}

static uint8_t znp_fcs(const uint8_t *f, size_t n)
{
    uint8_t x = 0;

    for (size_t i = 1; i < n; i++) {
        x ^= f[i];
    }
    return x;
}

// FE len cmd0 cmd1 data... FCS
static size_t znp_frame(uint8_t cmd0, uint8_t cmd1, const uint8_t *data, size_t n, uint8_t *out)
{
    out[0] = 0xFE;
    out[1] = (uint8_t)n;
    out[2] = cmd0;
    out[3] = cmd1;
    memcpy(out + 4, data, n);
    out[4 + n] = znp_fcs(out, 4 + n);
    return 5 + n;
}

static size_t znp_ping_srsp(uint8_t *out)
{
    const uint8_t caps[] = {0x79, 0x07};

    return znp_frame(0x61, 0x01, caps, sizeof(caps), out);
}

// TransportRev 2, product, 2.7.1, then `extra_n` more payload bytes (CodeRevision, bootloader fields).
static size_t znp_version_srsp(uint8_t product, const uint8_t *extra, size_t extra_n, uint8_t *out)
{
    uint8_t d[16] = {0x02, product, 2, 7, 1};

    if (extra_n) {
        memcpy(d + 5, extra, extra_n);
    }
    return znp_frame(0x61, 0x02, d, 5 + extra_n, out);
}

static const uint8_t REV_20598080[] = {0x40, 0x4D, 0x3A, 0x01};

static uint16_t ash_crc(const uint8_t *p, size_t n)
{
    uint16_t crc = 0xFFFF;

    for (size_t i = 0; i < n; i++) {
        crc ^= (uint16_t)(p[i] << 8);
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

static size_t ash_put(uint8_t *out, size_t at, uint8_t b)
{
    if (b == 0x7E || b == 0x11 || b == 0x13 || b == 0x18 || b == 0x1A || b == 0x7D) {
        out[at++] = 0x7D;
        b ^= 0x20;
    }
    out[at++] = b;
    return at;
}

// C1 <version> <code> CRC CRC 7E with byte stuffing.
static size_t ash_rstack(uint8_t version, uint8_t code, int crc_flip, uint8_t *out)
{
    const uint8_t body[] = {0xC1, version, code};
    uint16_t crc = (uint16_t)(ash_crc(body, sizeof(body)) ^ crc_flip);
    size_t at = 0;

    for (size_t i = 0; i < sizeof(body); i++) {
        at = ash_put(out, at, body[i]);
    }
    at = ash_put(out, at, (uint8_t)(crc >> 8));
    at = ash_put(out, at, (uint8_t)crc);
    out[at++] = 0x7E;
    return at;
}

// Walks the sequence with silence up to the given step (0 = spinel 460800 ... 4 = EZSP 460800); returns that step's
// first output.
static out_t at_step(int step)
{
    out_t o = begin();

    for (int i = 0; i < step; i++) {
        o = timeout_of(&o);
    }
    return o;
}

TEST_CASE("At the start: asks for spinel NCP_VERSION at 460800", "[radio_mux]")
{
    out_t o = begin();

    TEST_ASSERT_EQUAL(RADIO_MUX_PROBE_SEND, o.act);
    TEST_ASSERT_EQUAL_UINT32(460800, o.baud);
    assert_spinel_get(&o, PROP_NCP_VERSION, sizeof(PROP_NCP_VERSION));
}

TEST_CASE("At the start: the step ends 350 ms later", "[radio_mux]")
{
    out_t o = begin();

    TEST_ASSERT_EQUAL_INT64(T0 + 350, o.until);
}

TEST_CASE("On no answer within the step's share of the budget: waits on before the deadline", "[radio_mux]")
{
    out_t o = begin();
    out_t w = call(T0 + 100, NULL, 0);

    TEST_ASSERT_EQUAL(RADIO_MUX_PROBE_SEND, w.act);
    TEST_ASSERT_EQUAL_UINT32(0, w.tx_n);
    TEST_ASSERT_EQUAL_INT64(o.until, w.until);
    TEST_ASSERT_EQUAL_UINT32(460800, w.baud);
}

TEST_CASE("On no answer within the step's share of the budget: moves to spinel at 115200 for 350 ms", "[radio_mux]")
{
    out_t o = at_step(1);

    TEST_ASSERT_EQUAL(RADIO_MUX_PROBE_SEND, o.act);
    TEST_ASSERT_EQUAL_UINT32(115200, o.baud);
    assert_spinel_get(&o, PROP_NCP_VERSION, sizeof(PROP_NCP_VERSION));
    TEST_ASSERT_EQUAL_INT64(T0 + 700, o.until);
}

TEST_CASE("On no answer within the step's share of the budget: moves to ZNP SYS_PING at 115200 for 400 ms",
          "[radio_mux]")
{
    const uint8_t ping[] = {0xFE, 0x00, 0x21, 0x01, 0x20};
    out_t o = at_step(2);

    TEST_ASSERT_EQUAL(RADIO_MUX_PROBE_SEND, o.act);
    TEST_ASSERT_EQUAL_UINT32(115200, o.baud);
    TEST_ASSERT_EQUAL_UINT32(sizeof(ping), o.tx_n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(ping, o.tx, sizeof(ping));
    TEST_ASSERT_EQUAL_INT64(T0 + 1100, o.until);
}

TEST_CASE("On no answer within the step's share of the budget: moves to EZSP RST at 115200 for 800 ms", "[radio_mux]")
{
    const uint8_t rst[] = {0x1A, 0xC0, 0x38, 0xBC, 0x7E};
    out_t o = at_step(3);

    TEST_ASSERT_EQUAL(RADIO_MUX_PROBE_SEND, o.act);
    TEST_ASSERT_EQUAL_UINT32(115200, o.baud);
    TEST_ASSERT_EQUAL_UINT32(sizeof(rst), o.tx_n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(rst, o.tx, sizeof(rst));
    TEST_ASSERT_EQUAL_INT64(T0 + 1900, o.until);
}

TEST_CASE("On no answer within the step's share of the budget: moves to EZSP RST at 460800 for 800 ms", "[radio_mux]")
{
    const uint8_t rst[] = {0x1A, 0xC0, 0x38, 0xBC, 0x7E};
    out_t o = at_step(4);

    TEST_ASSERT_EQUAL(RADIO_MUX_PROBE_SEND, o.act);
    TEST_ASSERT_EQUAL_UINT32(460800, o.baud);
    TEST_ASSERT_EQUAL_UINT32(sizeof(rst), o.tx_n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(rst, o.tx, sizeof(rst));
    TEST_ASSERT_EQUAL_INT64(T0 + 2700, o.until);
}

TEST_CASE("On no answer within the step's share of the budget: finishes with unknown after the last step",
          "[radio_mux]")
{
    out_t o = at_step(5);

    TEST_ASSERT_EQUAL(RADIO_MUX_PROBE_DONE, o.act);
    TEST_ASSERT_EQUAL(RADIO_MUX_FW_UNKNOWN, o.rec.type);
    TEST_ASSERT_EQUAL_UINT32(0, o.rec.baud);
    TEST_ASSERT_EQUAL_STRING("", o.rec.version);
    TEST_ASSERT_EQUAL_STRING("", o.rec.chip);
}

TEST_CASE("On a spinel answer: asks for SPINEL_PROP_VENDOR_ZIGGURAT with a GET at the same baud for 200 ms",
          "[radio_mux]")
{
    uint8_t rx[160];
    size_t n = ncp_version_answer(KOENKK, rx, sizeof(rx));
    out_t first = begin();
    out_t o = call(T0 + 40, rx, n);

    TEST_ASSERT_EQUAL(RADIO_MUX_PROBE_SEND, o.act);
    TEST_ASSERT_EQUAL_UINT32(460800, o.baud);
    assert_spinel_get(&o, PROP_ZIGGURAT, sizeof(PROP_ZIGGURAT));
    TEST_ASSERT_EQUAL_INT64(T0 + 240, o.until);
    TEST_ASSERT_TRUE(first.until > 0);
}

TEST_CASE("On a spinel answer: the follow-up request is the GET bytes 81 02 DA 7A, never a SET", "[radio_mux]")
{
    const uint8_t want[] = {0x81, 0x02, 0xDA, 0x7A};
    uint8_t rx[160], f[16];
    size_t n = ncp_version_answer(KOENKK, rx, sizeof(rx));
    out_t o;

    begin();
    o = call(T0 + 40, rx, n);
    n = spinel_unwire(o.tx, o.tx_n, f, sizeof(f));
    TEST_ASSERT_EQUAL_UINT32(sizeof(want), n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, f, sizeof(want));
}

TEST_CASE("On a spinel answer: split over two reads is still taken", "[radio_mux]")
{
    uint8_t rx[160];
    size_t n = ncp_version_answer(KOENKK, rx, sizeof(rx));
    out_t o;

    begin();
    o = call(T0 + 10, rx, 7);
    TEST_ASSERT_EQUAL_UINT32(0, o.tx_n);
    o = call(T0 + 20, rx + 7, n - 7);
    assert_spinel_get(&o, PROP_ZIGGURAT, sizeof(PROP_ZIGGURAT));
}

TEST_CASE("On a spinel answer: PROP_VALUE_IS of the Ziggurat property finishes with ziggurat", "[radio_mux]")
{
    uint8_t rx[160], rz[40];
    size_t n = ncp_version_answer(KOENKK, rx, sizeof(rx));
    size_t nz = spinel_is(PROP_ZIGGURAT, sizeof(PROP_ZIGGURAT), NULL, 0, rz, sizeof(rz));
    out_t o;

    begin();
    call(T0 + 40, rx, n);
    o = call(T0 + 60, rz, nz);

    TEST_ASSERT_EQUAL(RADIO_MUX_PROBE_DONE, o.act);
    TEST_ASSERT_EQUAL(RADIO_MUX_FW_ZIGGURAT, o.rec.type);
    TEST_ASSERT_EQUAL_STRING("1.4.0-Koenkk-2025.3.1", o.rec.version);
    TEST_ASSERT_EQUAL_STRING("CC13XX_CC26XX", o.rec.chip);
    TEST_ASSERT_EQUAL_UINT32(460800, o.rec.baud);
}

TEST_CASE("On a spinel answer: a LAST_STATUS refusal finishes with spinel", "[radio_mux]")
{
    uint8_t rx[160], rz[40];
    size_t n = ncp_version_answer(KOENKK, rx, sizeof(rx));
    size_t nz = last_status_answer(rz, sizeof(rz));
    out_t o;

    begin();
    call(T0 + 40, rx, n);
    o = call(T0 + 60, rz, nz);

    TEST_ASSERT_EQUAL(RADIO_MUX_PROBE_DONE, o.act);
    TEST_ASSERT_EQUAL(RADIO_MUX_FW_SPINEL, o.rec.type);
    TEST_ASSERT_EQUAL_STRING("1.4.0-Koenkk-2025.3.1", o.rec.version);
    TEST_ASSERT_EQUAL_STRING("CC13XX_CC26XX", o.rec.chip);
    TEST_ASSERT_EQUAL_UINT32(460800, o.rec.baud);
}

TEST_CASE("On a spinel answer: no answer to the Ziggurat request in 200 ms finishes with spinel", "[radio_mux]")
{
    uint8_t rx[160];
    size_t n = ncp_version_answer(KOENKK, rx, sizeof(rx));
    out_t o;

    begin();
    o = call(T0 + 40, rx, n);
    o = call(o.until, NULL, 0);

    TEST_ASSERT_EQUAL(RADIO_MUX_PROBE_DONE, o.act);
    TEST_ASSERT_EQUAL(RADIO_MUX_FW_SPINEL, o.rec.type);
}

TEST_CASE("On a spinel answer: one at 115200 records that baud", "[radio_mux]")
{
    uint8_t rx[160], rz[40];
    size_t n = ncp_version_answer(KOENKK, rx, sizeof(rx));
    size_t nz = last_status_answer(rz, sizeof(rz));
    out_t o = at_step(1);

    o = call(o.until - 100, rx, n);
    TEST_ASSERT_EQUAL_UINT32(115200, o.baud);
    o = call(o.until - 100, rz, nz);

    TEST_ASSERT_EQUAL(RADIO_MUX_PROBE_DONE, o.act);
    TEST_ASSERT_EQUAL(RADIO_MUX_FW_SPINEL, o.rec.type);
    TEST_ASSERT_EQUAL_UINT32(115200, o.rec.baud);
}

TEST_CASE("On a spinel answer: a corrupt frame is ignored and the step goes on", "[radio_mux]")
{
    uint8_t rx[160];
    size_t n = ncp_version_answer(KOENKK, rx, sizeof(rx));
    out_t o;

    begin();
    rx[n - 3] ^= 0x01;
    o = call(T0 + 40, rx, n);
    TEST_ASSERT_EQUAL_UINT32(0, o.tx_n);
    o = call(T0 + 350, NULL, 0);
    TEST_ASSERT_EQUAL_UINT32(115200, o.baud);
}

TEST_CASE("On a ZNP SYS_PING reply: sends SYS_VERSION within the same step", "[radio_mux]")
{
    const uint8_t ver[] = {0xFE, 0x00, 0x21, 0x02, 0x23};
    uint8_t rx[24];
    size_t n = znp_ping_srsp(rx);
    out_t step = at_step(2);
    out_t o = call(step.until - 300, rx, n);

    TEST_ASSERT_EQUAL(RADIO_MUX_PROBE_SEND, o.act);
    TEST_ASSERT_EQUAL_UINT32(sizeof(ver), o.tx_n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(ver, o.tx, sizeof(ver));
    TEST_ASSERT_EQUAL_UINT32(115200, o.baud);
    TEST_ASSERT_EQUAL_INT64(step.until, o.until);
}

TEST_CASE("On a ZNP SYS_PING reply: no SYS_VERSION reply by the deadline finishes with znp and no version",
          "[radio_mux]")
{
    uint8_t rx[24];
    size_t n = znp_ping_srsp(rx);
    out_t step = at_step(2);
    out_t o = call(step.until - 300, rx, n);

    o = timeout_of(&o);
    TEST_ASSERT_EQUAL(RADIO_MUX_PROBE_DONE, o.act);
    TEST_ASSERT_EQUAL(RADIO_MUX_FW_ZNP, o.rec.type);
    TEST_ASSERT_EQUAL_STRING("", o.rec.version);
    TEST_ASSERT_EQUAL_UINT32(115200, o.rec.baud);
}

TEST_CASE("On a ZNP SYS_VERSION reply: finishes with znp, the variant and version from the reply, no chip", "[radio_mux]")
{
    uint8_t rx[24];
    size_t n = znp_version_srsp(1, NULL, 0, rx);
    out_t step = at_step(2);
    out_t o = call(step.until - 300, rx, n);

    TEST_ASSERT_EQUAL(RADIO_MUX_PROBE_DONE, o.act);
    TEST_ASSERT_EQUAL(RADIO_MUX_FW_ZNP, o.rec.type);
    TEST_ASSERT_EQUAL_STRING("Z-Stack 3.0.x 2.7.1", o.rec.version);
    TEST_ASSERT_EQUAL_STRING("", o.rec.chip);
    TEST_ASSERT_EQUAL_UINT32(115200, o.rec.baud);
}

TEST_CASE("On a ZNP SYS_VERSION reply: after the ping answer, in two reads", "[radio_mux]")
{
    uint8_t rp[24], rv[24];
    size_t np = znp_ping_srsp(rp), nv = znp_version_srsp(0, NULL, 0, rv);
    out_t step = at_step(2);
    out_t o = call(step.until - 300, rp, np);

    o = call(step.until - 200, rv, 3);
    TEST_ASSERT_EQUAL(RADIO_MUX_PROBE_SEND, o.act);
    o = call(step.until - 100, rv + 3, nv - 3);
    TEST_ASSERT_EQUAL(RADIO_MUX_PROBE_DONE, o.act);
    TEST_ASSERT_EQUAL_STRING("Z-Stack 1.2 2.7.1", o.rec.version);
}

TEST_CASE("On a ZNP SYS_VERSION reply: a CodeRevision is appended as rev <n>", "[radio_mux]")
{
    uint8_t rx[32];
    size_t n = znp_version_srsp(2, REV_20598080, sizeof(REV_20598080), rx);
    out_t step = at_step(2);
    out_t o = call(step.until - 300, rx, n);

    TEST_ASSERT_EQUAL(RADIO_MUX_FW_ZNP, o.rec.type);
    TEST_ASSERT_EQUAL_STRING("Z-Stack 3.x.0 2.7.1 rev 20598080", o.rec.version);
    TEST_ASSERT_EQUAL_STRING("", o.rec.chip);
}

TEST_CASE("On a ZNP SYS_VERSION reply: the longest payload (14 bytes, with bootloader fields) is accepted",
          "[radio_mux]")
{
    const uint8_t extra[] = {0x40, 0x4D, 0x3A, 0x01, 0x03, 0x11, 0x22, 0x33, 0x44};
    uint8_t rx[32];
    size_t n = znp_version_srsp(1, extra, sizeof(extra), rx);
    out_t step = at_step(2);
    out_t o = call(step.until - 300, rx, n);

    TEST_ASSERT_EQUAL_UINT32(19, n);
    TEST_ASSERT_EQUAL(RADIO_MUX_FW_ZNP, o.rec.type);
    TEST_ASSERT_EQUAL_STRING("Z-Stack 3.0.x 2.7.1 rev 20598080", o.rec.version);
}

TEST_CASE("On a ZNP SYS_VERSION reply: a payload of 10 bytes (CodeRevision and bootloader type) is accepted",
          "[radio_mux]")
{
    const uint8_t extra[] = {0x40, 0x4D, 0x3A, 0x01, 0x03};
    uint8_t rx[32];
    size_t n = znp_version_srsp(1, extra, sizeof(extra), rx);
    out_t step = at_step(2);
    out_t o = call(step.until - 300, rx, n);

    TEST_ASSERT_EQUAL(RADIO_MUX_FW_ZNP, o.rec.type);
    TEST_ASSERT_EQUAL_STRING("Z-Stack 3.0.x 2.7.1 rev 20598080", o.rec.version);
}

TEST_CASE("On a ZNP SYS_VERSION reply: a payload of 15 bytes is not an answer", "[radio_mux]")
{
    const uint8_t extra[10] = {0x40, 0x4D, 0x3A, 0x01};
    uint8_t rx[32];
    size_t n = znp_version_srsp(1, extra, sizeof(extra), rx);
    out_t step = at_step(2);
    out_t o = call(step.until - 300, rx, n);

    TEST_ASSERT_EQUAL(RADIO_MUX_PROBE_SEND, o.act);
}

TEST_CASE("On a ZNP SYS_VERSION reply: an unlisted product shows as Z-Stack product <n>", "[radio_mux]")
{
    uint8_t rx[24];
    size_t n = znp_version_srsp(9, NULL, 0, rx);
    out_t step = at_step(2);
    out_t o = call(step.until - 300, rx, n);

    TEST_ASSERT_EQUAL_STRING("Z-Stack product 9 2.7.1", o.rec.version);
    TEST_ASSERT_EQUAL_STRING("", o.rec.chip);
}

TEST_CASE("On a ZNP SYS_VERSION reply: a bad FCS is ignored and the step times out", "[radio_mux]")
{
    uint8_t rx[24];
    size_t n = znp_version_srsp(1, NULL, 0, rx);
    out_t step = at_step(2);
    out_t o;

    rx[n - 1] ^= 0xFF;
    o = call(step.until - 300, rx, n);
    TEST_ASSERT_EQUAL(RADIO_MUX_PROBE_SEND, o.act);
    TEST_ASSERT_EQUAL_UINT32(0, o.tx_n);
    o = timeout_of(&o);
    TEST_ASSERT_EQUAL(RADIO_MUX_PROBE_SEND, o.act);
    TEST_ASSERT_TRUE(o.tx_n > 0);
    TEST_ASSERT_EQUAL_HEX8(0x1A, o.tx[0]);
}

TEST_CASE("On a ZNP SYS_VERSION reply: noise before the frame is skipped", "[radio_mux]")
{
    uint8_t rx[32];
    size_t n = 0;
    out_t step = at_step(2);
    out_t o;

    rx[n++] = 0x00;
    rx[n++] = 0xFE; // a false start: a short length, then the real frame
    rx[n++] = 0x01;
    n += znp_version_srsp(1, NULL, 0, rx + n);
    o = call(step.until - 300, rx, n);
    TEST_ASSERT_EQUAL(RADIO_MUX_FW_ZNP, o.rec.type);
}

TEST_CASE("On an ASH RSTACK: finishes with ezsp and its version", "[radio_mux]")
{
    uint8_t rx[16];
    size_t n = ash_rstack(2, 0x02, 0, rx);
    out_t step = at_step(3);
    out_t o = call(step.until - 400, rx, n);

    TEST_ASSERT_EQUAL(RADIO_MUX_PROBE_DONE, o.act);
    TEST_ASSERT_EQUAL(RADIO_MUX_FW_EZSP, o.rec.type);
    TEST_ASSERT_EQUAL_STRING("ash v2", o.rec.version);
    TEST_ASSERT_EQUAL_UINT32(115200, o.rec.baud);
}

TEST_CASE("On an ASH RSTACK: one at 460800 records that baud", "[radio_mux]")
{
    uint8_t rx[16];
    size_t n = ash_rstack(2, 0x02, 0, rx);
    out_t step = at_step(4);
    out_t o = call(step.until - 400, rx, n);

    TEST_ASSERT_EQUAL(RADIO_MUX_FW_EZSP, o.rec.type);
    TEST_ASSERT_EQUAL_UINT32(460800, o.rec.baud);
}

TEST_CASE("On an ASH RSTACK: leading noise bytes are skipped", "[radio_mux]")
{
    uint8_t rx[24];
    size_t n = 0;
    out_t step = at_step(3);
    out_t o;

    rx[n++] = 0x00;
    rx[n++] = 0xFF;
    rx[n++] = 0x1A; // a cancel byte, as the device sends before a frame
    n += ash_rstack(2, 0x02, 0, rx + n);
    o = call(step.until - 400, rx, n);

    TEST_ASSERT_EQUAL(RADIO_MUX_FW_EZSP, o.rec.type);
    TEST_ASSERT_EQUAL_STRING("ash v2", o.rec.version);
}

TEST_CASE("On an ASH RSTACK: a stuffed byte in the frame is unstuffed before the CRC", "[radio_mux]")
{
    uint8_t rx[16];
    size_t n = ash_rstack(2, 0x11, 0, rx); // reset code 0x11 goes on the wire as 7D 31
    out_t step = at_step(3);
    out_t o = call(step.until - 400, rx, n);

    TEST_ASSERT_EQUAL_HEX8(0x7D, rx[2]);
    TEST_ASSERT_EQUAL(RADIO_MUX_FW_EZSP, o.rec.type);
}

TEST_CASE("On an ASH RSTACK: in two reads", "[radio_mux]")
{
    uint8_t rx[16];
    size_t n = ash_rstack(2, 0x02, 0, rx);
    out_t step = at_step(3);
    out_t o = call(step.until - 600, rx, 3);

    TEST_ASSERT_EQUAL(RADIO_MUX_PROBE_SEND, o.act);
    o = call(step.until - 400, rx + 3, n - 3);
    TEST_ASSERT_EQUAL(RADIO_MUX_FW_EZSP, o.rec.type);
}

TEST_CASE("On an ASH RSTACK: a bad CRC is ignored", "[radio_mux]")
{
    uint8_t rx[16];
    size_t n = ash_rstack(2, 0x02, 0x0100, rx);
    out_t step = at_step(3);
    out_t o = call(step.until - 400, rx, n);

    TEST_ASSERT_EQUAL(RADIO_MUX_PROBE_SEND, o.act);
    TEST_ASSERT_EQUAL_UINT32(0, o.tx_n);
}

TEST_CASE("On 3000 ms elapsed: finishes with unknown with every step silent", "[radio_mux]")
{
    out_t o = begin();
    int64_t last = T0;

    while (o.act == RADIO_MUX_PROBE_SEND) {
        TEST_ASSERT_TRUE(o.until <= T0 + 3000);
        last = o.until;
        o = call(last, NULL, 0);
    }
    TEST_ASSERT_EQUAL(RADIO_MUX_FW_UNKNOWN, o.rec.type);
    TEST_ASSERT_TRUE(last <= T0 + 3000);
}

TEST_CASE("On 3000 ms elapsed: finishes with unknown in the first step", "[radio_mux]")
{
    out_t o;

    begin();
    o = call(T0 + 3000, NULL, 0);
    TEST_ASSERT_EQUAL(RADIO_MUX_PROBE_DONE, o.act);
    TEST_ASSERT_EQUAL(RADIO_MUX_FW_UNKNOWN, o.rec.type);
}

TEST_CASE("On 3000 ms elapsed: finishes with unknown in the ZNP step, even with an answer in hand", "[radio_mux]")
{
    uint8_t rx[24];
    size_t n = znp_version_srsp(1, NULL, 0, rx);
    out_t o;

    at_step(2);
    o = call(T0 + 3000, rx, n);
    TEST_ASSERT_EQUAL(RADIO_MUX_PROBE_DONE, o.act);
    TEST_ASSERT_EQUAL(RADIO_MUX_FW_UNKNOWN, o.rec.type);
}

TEST_CASE("On 3000 ms elapsed: a late caller still gets unknown in the last step", "[radio_mux]")
{
    out_t o;

    at_step(4);
    o = call(T0 + 5000, NULL, 0);
    TEST_ASSERT_EQUAL(RADIO_MUX_PROBE_DONE, o.act);
    TEST_ASSERT_EQUAL(RADIO_MUX_FW_UNKNOWN, o.rec.type);
}

TEST_CASE("On 3000 ms elapsed: a late spinel answer's follow-up wait ends at the cap", "[radio_mux]")
{
    uint8_t rx[160];
    size_t n = ncp_version_answer(KOENKK, rx, sizeof(rx));
    out_t o;

    begin();
    o = call(T0 + 2950, rx, n);
    TEST_ASSERT_EQUAL_INT64(T0 + 3000, o.until);
    o = call(T0 + 3000, NULL, 0);
    TEST_ASSERT_EQUAL(RADIO_MUX_PROBE_DONE, o.act);
    TEST_ASSERT_EQUAL(RADIO_MUX_FW_SPINEL, o.rec.type);
}

TEST_CASE("On each probe step: new_step is true on the SEND that starts it", "[radio_mux]")
{
    out_t o = begin();

    TEST_ASSERT_TRUE(o.new_step);
    for (int i = 1; i < 5; i++) {
        o = timeout_of(&o);
        TEST_ASSERT_EQUAL(RADIO_MUX_PROBE_SEND, o.act);
        TEST_ASSERT_TRUE(o.new_step);
    }
}

TEST_CASE("On waiting inside a step: new_step is false", "[radio_mux]")
{
    out_t o;

    begin();
    o = call(T0 + 100, NULL, 0);
    TEST_ASSERT_FALSE(o.new_step);
}

TEST_CASE("On the Ziggurat follow-up: new_step is false", "[radio_mux]")
{
    uint8_t rx[160];
    size_t n = ncp_version_answer(KOENKK, rx, sizeof(rx));
    out_t o;

    begin();
    o = call(T0 + 40, rx, n);
    TEST_ASSERT_TRUE(o.tx_n > 0);
    TEST_ASSERT_FALSE(o.new_step);
}

TEST_CASE("On the SYS_VERSION follow-up: new_step is false", "[radio_mux]")
{
    uint8_t rx[24];
    size_t n = znp_ping_srsp(rx);
    out_t step = at_step(2);
    out_t o;

    TEST_ASSERT_TRUE(step.new_step);
    o = call(step.until - 300, rx, n);
    TEST_ASSERT_TRUE(o.tx_n > 0);
    TEST_ASSERT_FALSE(o.new_step);
}

TEST_CASE("On DONE: new_step is false", "[radio_mux]")
{
    out_t o = at_step(5);

    TEST_ASSERT_EQUAL(RADIO_MUX_PROBE_DONE, o.act);
    TEST_ASSERT_FALSE(o.new_step);
}

TEST_CASE("On an unsolicited spinel frame (TID 0) in the vendor stage: ignores it, then takes the real answer",
          "[radio_mux]")
{
    const uint8_t status[] = {0x00}, reset[] = {0x72};
    uint8_t rx[160], rn[40], rz[40];
    size_t n = ncp_version_answer(KOENKK, rx, sizeof(rx));
    size_t nn = spinel_is_hdr(0x80, status, 1, reset, 1, rn, sizeof(rn));
    size_t nz = spinel_is(PROP_ZIGGURAT, sizeof(PROP_ZIGGURAT), NULL, 0, rz, sizeof(rz));
    out_t o;

    begin();
    call(T0 + 40, rx, n);
    o = call(T0 + 50, rn, nn);
    TEST_ASSERT_EQUAL(RADIO_MUX_PROBE_SEND, o.act);
    TEST_ASSERT_EQUAL_UINT32(0, o.tx_n);
    o = call(T0 + 60, rz, nz);
    TEST_ASSERT_EQUAL(RADIO_MUX_PROBE_DONE, o.act);
    TEST_ASSERT_EQUAL(RADIO_MUX_FW_ZIGGURAT, o.rec.type);
}

TEST_CASE("On an unsolicited spinel frame (TID 0) before the version answer: ignores it", "[radio_mux]")
{
    uint8_t rx[160];
    size_t n = spinel_is_hdr(0x80, PROP_NCP_VERSION, 1, (const uint8_t *)"X/1.0; C", 9, rx, sizeof(rx));
    out_t o;

    begin();
    o = call(T0 + 40, rx, n);
    TEST_ASSERT_EQUAL(RADIO_MUX_PROBE_SEND, o.act);
    TEST_ASSERT_EQUAL_UINT32(0, o.tx_n);
}

TEST_CASE("On \"OPENTHREAD/1.4.0-Koenkk-2025.3.1; CC13XX_CC26XX; Mar 13 2025\": returns version and chip",
          "[radio_mux]")
{
    char v[48], c[24];

    TEST_ASSERT_TRUE(radio_mux_detect_parse_spinel_version(KOENKK, v, sizeof(v), c, sizeof(c)));
    TEST_ASSERT_EQUAL_STRING("1.4.0-Koenkk-2025.3.1", v);
    TEST_ASSERT_EQUAL_STRING("CC13XX_CC26XX", c);
}

TEST_CASE("On a string without '/': the first field is the version, the second the chip", "[radio_mux]")
{
    char v[48], c[24];

    TEST_ASSERT_TRUE(radio_mux_detect_parse_spinel_version("ZIGGURAT 0.3 ;  EFR32MG24 ", v, sizeof(v), c, sizeof(c)));
    TEST_ASSERT_EQUAL_STRING("ZIGGURAT 0.3", v);
    TEST_ASSERT_EQUAL_STRING("EFR32MG24", c);
}

TEST_CASE("On a string with one field: the chip is empty", "[radio_mux]")
{
    char v[48], c[24] = "junk";

    TEST_ASSERT_TRUE(radio_mux_detect_parse_spinel_version("OPENTHREAD/1.3.0", v, sizeof(v), c, sizeof(c)));
    TEST_ASSERT_EQUAL_STRING("1.3.0", v);
    TEST_ASSERT_EQUAL_STRING("", c);
}

TEST_CASE("On an empty or NULL string: returns false", "[radio_mux]")
{
    char v[48], c[24];

    TEST_ASSERT_FALSE(radio_mux_detect_parse_spinel_version("", v, sizeof(v), c, sizeof(c)));
    TEST_ASSERT_FALSE(radio_mux_detect_parse_spinel_version(NULL, v, sizeof(v), c, sizeof(c)));
}

TEST_CASE("On fields longer than the buffers: cuts them and keeps the NUL", "[radio_mux]")
{
    char v[8], c[5];

    TEST_ASSERT_TRUE(radio_mux_detect_parse_spinel_version(KOENKK, v, sizeof(v), c, sizeof(c)));
    TEST_ASSERT_EQUAL_STRING("1.4.0-K", v);
    TEST_ASSERT_EQUAL_STRING("CC13", c);
}
