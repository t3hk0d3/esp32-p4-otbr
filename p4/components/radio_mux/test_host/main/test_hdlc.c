#include <stdlib.h>
#include <string.h>
#include "radio_mux_hdlc.h"
#include "unity.h"
#include "unity_test_runner.h"

#define FLAG 0x7E
#define ESC 0x7D
#define MAX_FRAMES 8
#define FRAME_MAX 64

// Reference CRC-16/X.25: bitwise, reflected polynomial 0x8408, init 0xFFFF, xorout 0xFFFF.
static uint16_t ref_crc(const uint8_t *p, size_t n)
{
    uint16_t crc = 0xFFFF;

    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0x8408) : (uint16_t)(crc >> 1);
        }
    }
    return (uint16_t)~crc;
}

static bool needs_escape(uint8_t b)
{
    return b == 0x7E || b == 0x7D || b == 0x11 || b == 0x13 || b == 0xF8;
}

static size_t put_escaped(uint8_t *out, size_t at, uint8_t b)
{
    if (needs_escape(b)) {
        out[at++] = ESC;
        out[at++] = b ^ 0x20;
    } else {
        out[at++] = b;
    }
    return at;
}

// The expected encoding, built from the reference CRC.
static size_t ref_encode(const uint8_t *frame, size_t n, uint8_t *out)
{
    uint16_t crc = ref_crc(frame, n);
    size_t at = 0;

    out[at++] = FLAG;
    for (size_t i = 0; i < n; i++) {
        at = put_escaped(out, at, frame[i]);
    }
    at = put_escaped(out, at, crc & 0xFF);
    at = put_escaped(out, at, crc >> 8);
    out[at++] = FLAG;
    return at;
}

typedef struct {
    uint8_t data[MAX_FRAMES][FRAME_MAX];
    size_t len[MAX_FRAMES];
    size_t n;
} frames_t;

// Feeds `in` in chunks of `chunk` bytes, collecting every completed frame and handing the decoder the same buffer
// again.
static void feed_chunks(radio_mux_hdlc_dec_t *d, uint8_t *buf, size_t cap, const uint8_t *in, size_t n, size_t chunk,
                        frames_t *out)
{
    size_t at = 0;

    while (at < n) {
        size_t take = n - at < chunk ? n - at : chunk;
        size_t used = 0;

        while (used < take) {
            bool done = false;

            used += radio_mux_hdlc_decode_feed(d, in + at + used, take - used, &done);
            if (done) {
                TEST_ASSERT_LESS_THAN(MAX_FRAMES, out->n);
                TEST_ASSERT_LESS_OR_EQUAL(FRAME_MAX, d->len);
                memcpy(out->data[out->n], d->buf, d->len);
                out->len[out->n++] = d->len;
                radio_mux_hdlc_dec_set_buf(d, buf, cap);
            }
        }
        at += take;
    }
}

static const uint8_t SPINEL_GET_PROTOCOL_VERSION[] = {0x81, 0x02, 0x01};

TEST_CASE("On the ASCII input 123456789: the reference CRC-16/X.25 is the standard check value 0x906E", "[radio_mux]")
{
    TEST_ASSERT_EQUAL_HEX16(0x906E, ref_crc((const uint8_t *)"123456789", 9));
}

TEST_CASE("On a spinel request: matches the golden frame", "[radio_mux]")
{
    uint8_t out[32], want[32];
    uint16_t crc = ref_crc(SPINEL_GET_PROTOCOL_VERSION, sizeof(SPINEL_GET_PROTOCOL_VERSION));
    size_t want_n = ref_encode(SPINEL_GET_PROTOCOL_VERSION, sizeof(SPINEL_GET_PROTOCOL_VERSION), want);
    size_t n = radio_mux_hdlc_encode(SPINEL_GET_PROTOCOL_VERSION, sizeof(SPINEL_GET_PROTOCOL_VERSION), out, sizeof(out));

    TEST_ASSERT_EQUAL_UINT8(FLAG, want[0]);
    TEST_ASSERT_EQUAL_UINT8(0x81, want[1]);
    TEST_ASSERT_EQUAL_UINT8(0x02, want[2]);
    TEST_ASSERT_EQUAL_UINT8(0x01, want[3]);
    TEST_ASSERT_EQUAL_UINT8(crc & 0xFF, want[4]);
    TEST_ASSERT_EQUAL_UINT8(crc >> 8, want[5]);
    TEST_ASSERT_EQUAL_UINT8(FLAG, want[6]);
    TEST_ASSERT_EQUAL_UINT32(want_n, n);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, out, n);
}

TEST_CASE("On a frame containing 0x7E, 0x7D, 0x11, 0x13, 0xF8: escapes each as 0x7D, byte ^ 0x20",
          "[radio_mux]")
{
    const uint8_t frame[] = {0x01, 0x7E, 0x7D, 0x11, 0x13, 0xF8, 0x02};
    const uint8_t escaped[] = {0x01, 0x7D, 0x5E, 0x7D, 0x5D, 0x7D, 0x31, 0x7D, 0x33, 0x7D, 0xD8, 0x02};
    uint8_t out[64];
    size_t n = radio_mux_hdlc_encode(frame, sizeof(frame), out, sizeof(out));

    TEST_ASSERT_GREATER_THAN(sizeof(escaped) + 2, n);
    TEST_ASSERT_EQUAL_UINT8(FLAG, out[0]);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(escaped, out + 1, sizeof(escaped));
}

TEST_CASE("On a frame containing 0x7E, 0x7D, 0x11, 0x13, 0xF8: appends the CRC-16/X.25 little-endian, "
          "wraps in 0x7E",
          "[radio_mux]")
{
    const uint8_t frame[] = {0x01, 0x7E, 0x7D, 0x11, 0x13, 0xF8, 0x02};
    uint8_t out[64], want[64];
    size_t want_n = ref_encode(frame, sizeof(frame), want);
    size_t n = radio_mux_hdlc_encode(frame, sizeof(frame), out, sizeof(out));

    TEST_ASSERT_EQUAL_UINT32(want_n, n);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, out, n);
    TEST_ASSERT_EQUAL_UINT8(FLAG, out[n - 1]);
}

TEST_CASE("On a frame whose CRC holds an escaped byte: escapes the CRC bytes too", "[radio_mux]")
{
    uint8_t frame[1] = {0};
    uint8_t out[16], want[16];
    bool found = false;
    size_t want_n, n;

    for (int v = 0; v < 256 && !found; v++) {
        uint16_t crc;

        frame[0] = (uint8_t)v;
        crc = ref_crc(frame, 1);
        found = needs_escape(crc & 0xFF) || needs_escape(crc >> 8);
    }
    TEST_ASSERT_TRUE(found);

    want_n = ref_encode(frame, 1, want);
    n = radio_mux_hdlc_encode(frame, 1, out, sizeof(out));

    TEST_ASSERT_GREATER_THAN(1 + 1 + 2 + 1, n);
    TEST_ASSERT_EQUAL_UINT32(want_n, n);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, out, n);
}

TEST_CASE("On an output buffer exactly as large as the encoding: writes it whole", "[radio_mux]")
{
    uint8_t out[7], want[7];
    size_t want_n = ref_encode(SPINEL_GET_PROTOCOL_VERSION, sizeof(SPINEL_GET_PROTOCOL_VERSION), want);

    TEST_ASSERT_EQUAL_UINT32(sizeof(out), want_n);
    TEST_ASSERT_EQUAL_UINT32(want_n, radio_mux_hdlc_encode(SPINEL_GET_PROTOCOL_VERSION,
                                                           sizeof(SPINEL_GET_PROTOCOL_VERSION), out, sizeof(out)));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, out, want_n);
}

TEST_CASE("On an output buffer too small: returns 0", "[radio_mux]")
{
    const uint8_t frame[] = {0x7E, 0x01, 0x7D};
    uint8_t out[64];
    size_t full = radio_mux_hdlc_encode(frame, sizeof(frame), out, sizeof(out));

    TEST_ASSERT_GREATER_THAN(0, full);
    for (size_t cap = 0; cap < full; cap++) {
        TEST_ASSERT_EQUAL_UINT32(0, radio_mux_hdlc_encode(frame, sizeof(frame), out, cap));
    }
}

TEST_CASE("On an output buffer too small: writes nothing past its end", "[radio_mux]")
{
    const uint8_t frame[] = {0x7E, 0x01, 0x7D};
    uint8_t out[64], all_sentinel[64];
    size_t full = radio_mux_hdlc_encode(frame, sizeof(frame), out, sizeof(out));

    memset(all_sentinel, 0xA5, sizeof(all_sentinel));
    for (size_t cap = 0; cap < full; cap++) {
        memset(out, 0xA5, sizeof(out));
        radio_mux_hdlc_encode(frame, sizeof(frame), out, cap);
        TEST_ASSERT_EQUAL_UINT8_ARRAY(all_sentinel, out, sizeof(out));
    }
}

TEST_CASE("On a frame split across several chunks: emits it once, complete", "[radio_mux]")
{
    const uint8_t frame[] = {0x81, 0x7E, 0x02, 0x7D, 0x11, 0xF8, 0x13, 0x05};
    uint8_t enc[64], buf[32];
    size_t n = radio_mux_hdlc_encode(frame, sizeof(frame), enc, sizeof(enc));

    for (size_t chunk = 1; chunk <= 4; chunk++) {
        radio_mux_hdlc_dec_t d;
        frames_t got = {.n = 0};

        radio_mux_hdlc_dec_init(&d, buf, sizeof(buf));
        feed_chunks(&d, buf, sizeof(buf), enc, n, chunk, &got);

        TEST_ASSERT_EQUAL_UINT32(1, got.n);
        TEST_ASSERT_EQUAL_UINT32(sizeof(frame), got.len[0]);
        TEST_ASSERT_EQUAL_UINT8_ARRAY(frame, got.data[0], sizeof(frame));
    }
}

TEST_CASE("On a frame split across several chunks: strips the CRC", "[radio_mux]")
{
    uint8_t enc[64], buf[32];
    radio_mux_hdlc_dec_t d;
    bool done = false;
    size_t n = radio_mux_hdlc_encode(SPINEL_GET_PROTOCOL_VERSION, sizeof(SPINEL_GET_PROTOCOL_VERSION), enc,
                                     sizeof(enc));

    radio_mux_hdlc_dec_init(&d, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_UINT32(n, radio_mux_hdlc_decode_feed(&d, enc, n, &done));

    TEST_ASSERT_TRUE(done);
    TEST_ASSERT_EQUAL_UINT32(sizeof(SPINEL_GET_PROTOCOL_VERSION), d.len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(SPINEL_GET_PROTOCOL_VERSION, d.buf, d.len);
}

TEST_CASE("On two frames in one input: stops after the first and consumes only it", "[radio_mux]")
{
    const uint8_t a[] = {0x81, 0x02, 0x01}, b[] = {0x82, 0x03};
    uint8_t enc[64], buf[32];
    radio_mux_hdlc_dec_t d;
    bool done = false;
    size_t na = radio_mux_hdlc_encode(a, sizeof(a), enc, sizeof(enc));
    size_t nb = radio_mux_hdlc_encode(b, sizeof(b), enc + na, sizeof(enc) - na);
    size_t used;

    radio_mux_hdlc_dec_init(&d, buf, sizeof(buf));
    used = radio_mux_hdlc_decode_feed(&d, enc, na + nb, &done);

    TEST_ASSERT_TRUE(done);
    TEST_ASSERT_EQUAL_UINT32(na, used);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(a, d.buf, d.len);

    radio_mux_hdlc_dec_set_buf(&d, buf, sizeof(buf));
    done = false;
    TEST_ASSERT_EQUAL_UINT32(nb, radio_mux_hdlc_decode_feed(&d, enc + used, na + nb - used, &done));
    TEST_ASSERT_TRUE(done);
    TEST_ASSERT_EQUAL_UINT32(sizeof(b), d.len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(b, d.buf, d.len);
}

TEST_CASE("On an input that ends inside a frame: reports no frame and consumes it all", "[radio_mux]")
{
    uint8_t enc[64], buf[32];
    radio_mux_hdlc_dec_t d;
    bool done = true;
    size_t n = radio_mux_hdlc_encode(SPINEL_GET_PROTOCOL_VERSION, sizeof(SPINEL_GET_PROTOCOL_VERSION), enc,
                                     sizeof(enc));

    radio_mux_hdlc_dec_init(&d, buf, sizeof(buf));

    TEST_ASSERT_EQUAL_UINT32(n - 2, radio_mux_hdlc_decode_feed(&d, enc, n - 2, &done));
    TEST_ASSERT_FALSE(done);
}

TEST_CASE("On an escaped byte outside the encoder's set: accepts it", "[radio_mux]")
{
    // 0x7D 0x41 is 0x61; the CRC is escaped by hand only where it needs it.
    const uint8_t frame[] = {0x61, 0x02};
    uint8_t wire[16], buf[16];
    radio_mux_hdlc_dec_t d;
    bool done = false;
    size_t at = 0;
    uint16_t crc = ref_crc(frame, sizeof(frame));

    wire[at++] = FLAG;
    wire[at++] = ESC;
    wire[at++] = 0x41;
    wire[at++] = 0x02;
    at = put_escaped(wire, at, crc & 0xFF);
    at = put_escaped(wire, at, crc >> 8);
    wire[at++] = FLAG;

    radio_mux_hdlc_dec_init(&d, buf, sizeof(buf));
    radio_mux_hdlc_decode_feed(&d, wire, at, &done);

    TEST_ASSERT_TRUE(done);
    TEST_ASSERT_EQUAL_UINT32(sizeof(frame), d.len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(frame, d.buf, d.len);
}

TEST_CASE("On a bad CRC: drops the frame", "[radio_mux]")
{
    const uint8_t frame[] = {0x81, 0x02, 0x01, 0x40};
    uint8_t enc[64], buf[32];
    radio_mux_hdlc_dec_t d;
    frames_t got = {.n = 0};
    size_t n = radio_mux_hdlc_encode(frame, sizeof(frame), enc, sizeof(enc));

    enc[2] ^= 0x01; // a payload bit flips on the wire
    radio_mux_hdlc_dec_init(&d, buf, sizeof(buf));
    feed_chunks(&d, buf, sizeof(buf), enc, n, n, &got);

    TEST_ASSERT_EQUAL_UINT32(0, got.n);
}

TEST_CASE("On a bad CRC: counts it in crc_errors and decodes the next frame", "[radio_mux]")
{
    const uint8_t bad[] = {0x81, 0x02, 0x01, 0x40}, good[] = {0x82, 0x05};
    uint8_t enc[64], buf[32];
    radio_mux_hdlc_dec_t d;
    frames_t got = {.n = 0};
    size_t nb = radio_mux_hdlc_encode(bad, sizeof(bad), enc, sizeof(enc));
    size_t ng;

    enc[2] ^= 0x01;
    ng = radio_mux_hdlc_encode(good, sizeof(good), enc + nb, sizeof(enc) - nb);
    radio_mux_hdlc_dec_init(&d, buf, sizeof(buf));
    feed_chunks(&d, buf, sizeof(buf), enc, nb + ng, 3, &got);

    TEST_ASSERT_EQUAL_UINT32(1, d.crc_errors);
    TEST_ASSERT_EQUAL_UINT32(0, d.overruns);
    TEST_ASSERT_EQUAL_UINT32(1, got.n);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(good, got.data[0], sizeof(good));
}

TEST_CASE("On a frame shorter than the CRC: drops it and counts it in crc_errors", "[radio_mux]")
{
    const uint8_t wire[] = {FLAG, 0x01, FLAG};
    uint8_t buf[16];
    radio_mux_hdlc_dec_t d;
    frames_t got = {.n = 0};

    radio_mux_hdlc_dec_init(&d, buf, sizeof(buf));
    feed_chunks(&d, buf, sizeof(buf), wire, sizeof(wire), sizeof(wire), &got);

    TEST_ASSERT_EQUAL_UINT32(0, got.n);
    TEST_ASSERT_EQUAL_UINT32(1, d.crc_errors);
}

TEST_CASE("On a frame aborted by 0x7D 0x7E: drops it and counts it in crc_errors", "[radio_mux]")
{
    const uint8_t wire[] = {FLAG, 0x81, 0x02, ESC, FLAG};
    const uint8_t good[] = {0x83, 0x01};
    uint8_t enc[16], buf[16];
    radio_mux_hdlc_dec_t d;
    frames_t got = {.n = 0};
    size_t n = radio_mux_hdlc_encode(good, sizeof(good), enc, sizeof(enc));

    radio_mux_hdlc_dec_init(&d, buf, sizeof(buf));
    feed_chunks(&d, buf, sizeof(buf), wire, sizeof(wire), sizeof(wire), &got);
    TEST_ASSERT_EQUAL_UINT32(0, got.n);
    TEST_ASSERT_EQUAL_UINT32(1, d.crc_errors);

    feed_chunks(&d, buf, sizeof(buf), enc, n, n, &got);
    TEST_ASSERT_EQUAL_UINT32(1, got.n);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(good, got.data[0], sizeof(good));
}

TEST_CASE("On a frame longer than the slot: drops it", "[radio_mux]")
{
    uint8_t frame[20], enc[64], buf[8];
    radio_mux_hdlc_dec_t d;
    frames_t got = {.n = 0};
    size_t n;

    for (size_t i = 0; i < sizeof(frame); i++) {
        frame[i] = (uint8_t)(0x20 + i);
    }
    n = radio_mux_hdlc_encode(frame, sizeof(frame), enc, sizeof(enc));
    radio_mux_hdlc_dec_init(&d, buf, sizeof(buf));
    feed_chunks(&d, buf, sizeof(buf), enc, n, n, &got);

    TEST_ASSERT_EQUAL_UINT32(0, got.n);
}

TEST_CASE("On a frame longer than the slot: counts it in overruns once, resynchronises on the next 0x7E",
          "[radio_mux]")
{
    uint8_t frame[20], good[] = {0x81, 0x02, 0x01}, enc[96], buf[8];
    radio_mux_hdlc_dec_t d;
    frames_t got = {.n = 0};
    size_t n, ng;

    for (size_t i = 0; i < sizeof(frame); i++) {
        frame[i] = (uint8_t)(0x20 + i);
    }
    n = radio_mux_hdlc_encode(frame, sizeof(frame), enc, sizeof(enc));
    ng = radio_mux_hdlc_encode(good, sizeof(good), enc + n, sizeof(enc) - n);
    radio_mux_hdlc_dec_init(&d, buf, sizeof(buf));
    feed_chunks(&d, buf, sizeof(buf), enc, n + ng, 5, &got);

    TEST_ASSERT_EQUAL_UINT32(1, d.overruns);
    TEST_ASSERT_EQUAL_UINT32(0, d.crc_errors);
    TEST_ASSERT_EQUAL_UINT32(1, got.n);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(good, got.data[0], sizeof(good));
}

TEST_CASE("On a frame of exactly slot - 2 bytes: still fits; one more byte overruns", "[radio_mux]")
{
    uint8_t frame[7] = {1, 2, 3, 4, 5, 6, 7}, enc[64], buf[8];
    radio_mux_hdlc_dec_t d;
    frames_t got = {.n = 0};
    size_t n = radio_mux_hdlc_encode(frame, 6, enc, sizeof(enc));

    radio_mux_hdlc_dec_init(&d, buf, sizeof(buf));
    feed_chunks(&d, buf, sizeof(buf), enc, n, n, &got);
    TEST_ASSERT_EQUAL_UINT32(1, got.n);
    TEST_ASSERT_EQUAL_UINT32(6, got.len[0]);
    TEST_ASSERT_EQUAL_UINT32(0, d.overruns);

    n = radio_mux_hdlc_encode(frame, 7, enc, sizeof(enc));
    feed_chunks(&d, buf, sizeof(buf), enc, n, n, &got);
    TEST_ASSERT_EQUAL_UINT32(1, got.n);
    TEST_ASSERT_EQUAL_UINT32(1, d.overruns);
}

TEST_CASE("On repeated 0x7E between frames: emits nothing for the empty frames", "[radio_mux]")
{
    const uint8_t a[] = {0x81, 0x02, 0x01}, b[] = {0x82, 0x03};
    uint8_t enc[64], buf[16];
    radio_mux_hdlc_dec_t d;
    frames_t got = {.n = 0};
    size_t at = 0;

    enc[at++] = FLAG;
    enc[at++] = FLAG;
    enc[at++] = FLAG;
    at += radio_mux_hdlc_encode(a, sizeof(a), enc + at, sizeof(enc) - at);
    enc[at++] = FLAG;
    enc[at++] = FLAG;
    at += radio_mux_hdlc_encode(b, sizeof(b), enc + at, sizeof(enc) - at);
    enc[at++] = FLAG;
    radio_mux_hdlc_dec_init(&d, buf, sizeof(buf));
    feed_chunks(&d, buf, sizeof(buf), enc, at, 4, &got);

    TEST_ASSERT_EQUAL_UINT32(2, got.n);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(a, got.data[0], sizeof(a));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(b, got.data[1], sizeof(b));
    TEST_ASSERT_EQUAL_UINT32(0, d.crc_errors);
    TEST_ASSERT_EQUAL_UINT32(0, d.overruns);
}

TEST_CASE("On bytes before the first 0x7E: ignores them", "[radio_mux]")
{
    uint8_t enc[64], buf[16];
    radio_mux_hdlc_dec_t d;
    frames_t got = {.n = 0};
    size_t at = 0;

    enc[at++] = 0x55;
    enc[at++] = 0x66;
    at += radio_mux_hdlc_encode(SPINEL_GET_PROTOCOL_VERSION, sizeof(SPINEL_GET_PROTOCOL_VERSION), enc + at,
                                sizeof(enc) - at);
    radio_mux_hdlc_dec_init(&d, buf, sizeof(buf));
    feed_chunks(&d, buf, sizeof(buf), enc, at, at, &got);

    TEST_ASSERT_EQUAL_UINT32(1, got.n);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(SPINEL_GET_PROTOCOL_VERSION, got.data[0], sizeof(SPINEL_GET_PROTOCOL_VERSION));
    TEST_ASSERT_EQUAL_UINT32(0, d.crc_errors);
}

TEST_CASE("On a byte stream holding two frames and half of a third: yields the two frames with their flags",
          "[radio_mux]")
{
    const uint8_t f1[] = {0x81, 0x02, 0x01}, f2[] = {0x7E, 0x02}, f3[] = {0x83, 0x04, 0x05, 0x06};
    uint8_t s[128], e1[16], e2[16], e3[16];
    size_t n1 = radio_mux_hdlc_encode(f1, sizeof(f1), e1, sizeof(e1));
    size_t n2 = radio_mux_hdlc_encode(f2, sizeof(f2), e2, sizeof(e2));
    size_t n3 = radio_mux_hdlc_encode(f3, sizeof(f3), e3, sizeof(e3));
    size_t at = 0, flen = 0, used;

    memcpy(s + at, e1, n1);
    at += n1;
    memcpy(s + at, e2, n2);
    at += n2;
    memcpy(s + at, e3, n3 - 3); // the third stops short of its CRC and closing flag
    at += n3 - 3;

    used = radio_mux_hdlc_chunk(s, at, &flen);
    TEST_ASSERT_EQUAL_UINT32(n1, flen);
    TEST_ASSERT_EQUAL_UINT32(n1 - 1, used);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(e1, s + used - flen + 1, flen);

    memmove(s, s + used, at - used);
    at -= used;
    used = radio_mux_hdlc_chunk(s, at, &flen);
    TEST_ASSERT_EQUAL_UINT32(n2, flen);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(e2, s + used - flen + 1, flen);
}

TEST_CASE("On a byte stream holding two frames and half of a third: keeps the half for the next call",
          "[radio_mux]")
{
    const uint8_t f1[] = {0x81, 0x02, 0x01}, f2[] = {0x7E, 0x02}, f3[] = {0x83, 0x04, 0x05, 0x06};
    uint8_t s[128], e1[16], e2[16], e3[16];
    size_t n1 = radio_mux_hdlc_encode(f1, sizeof(f1), e1, sizeof(e1));
    size_t n2 = radio_mux_hdlc_encode(f2, sizeof(f2), e2, sizeof(e2));
    size_t n3 = radio_mux_hdlc_encode(f3, sizeof(f3), e3, sizeof(e3));
    size_t at = 0, flen = 0, used;

    memcpy(s + at, e1, n1);
    at += n1;
    memcpy(s + at, e2, n2);
    at += n2;
    memcpy(s + at, e3, n3 - 3);
    at += n3 - 3;
    for (int i = 0; i < 2; i++) {
        used = radio_mux_hdlc_chunk(s, at, &flen);
        memmove(s, s + used, at - used);
        at -= used;
    }

    // Nothing complete left: the closing flag of the second frame and the half of the third stay.
    TEST_ASSERT_EQUAL_UINT32(0, radio_mux_hdlc_chunk(s, at, &flen));
    TEST_ASSERT_EQUAL_UINT32(0, flen);
    TEST_ASSERT_EQUAL_UINT32(1 + n3 - 3, at);
    TEST_ASSERT_EQUAL_UINT8(FLAG, s[0]);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(e3, s + 1, n3 - 3);

    // The rest arrives: the third frame is complete.
    memcpy(s + at, e3 + n3 - 3, 3);
    at += 3;
    used = radio_mux_hdlc_chunk(s, at, &flen);
    TEST_ASSERT_EQUAL_UINT32(n3, flen);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(e3, s + used - flen + 1, flen);
}

TEST_CASE("On repeated flags and bytes before the first flag: skips them", "[radio_mux]")
{
    uint8_t s[32], e[16];
    size_t ne = radio_mux_hdlc_encode(SPINEL_GET_PROTOCOL_VERSION, sizeof(SPINEL_GET_PROTOCOL_VERSION), e, sizeof(e));
    size_t at = 0, flen = 0, used;

    s[at++] = 0x55;
    s[at++] = FLAG;
    s[at++] = FLAG;
    memcpy(s + at, e, ne);
    at += ne;
    used = radio_mux_hdlc_chunk(s, at, &flen);

    TEST_ASSERT_EQUAL_UINT32(ne, flen);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(e, s + used - flen + 1, flen);
    TEST_ASSERT_EQUAL_UINT32(at - 1, used);
}

TEST_CASE("On a stream with no complete frame: returns 0", "[radio_mux]")
{
    const uint8_t only_flag[] = {FLAG}, two_flags[] = {FLAG, FLAG}, open[] = {FLAG, 0x01, 0x02};
    size_t flen = 99;

    TEST_ASSERT_EQUAL_UINT32(0, radio_mux_hdlc_chunk(only_flag, sizeof(only_flag), &flen));
    TEST_ASSERT_EQUAL_UINT32(0, flen);
    TEST_ASSERT_EQUAL_UINT32(0, radio_mux_hdlc_chunk(two_flags, sizeof(two_flags), &flen));
    TEST_ASSERT_EQUAL_UINT32(0, radio_mux_hdlc_chunk(open, sizeof(open), &flen));
    TEST_ASSERT_EQUAL_UINT32(0, radio_mux_hdlc_chunk(open, 0, &flen));
}
