#include <stdio.h>
#include <string.h>
#include "radio_mux_detect.h"

enum {
    ST_START,
    ST_SPINEL,     // NCP_VERSION asked
    ST_VENDOR,     // spinel seen, the Ziggurat property asked
    ST_ZNP_PING,   // SYS_PING sent
    ST_ZNP_VER,    // ZNP seen, SYS_VERSION sent
    ST_ASH,        // RST sent
    ST_DONE,
};

typedef enum { KIND_SPINEL, KIND_ZNP, KIND_ASH } kind_t;

typedef struct {
    uint32_t baud;
    kind_t kind;
    int64_t budget_ms;
} step_t;

// The probe order of the Global Constraints.
static const step_t STEPS[] = {
    {460800, KIND_SPINEL, RADIO_MUX_DETECT_SPINEL_MS},
    {115200, KIND_SPINEL, RADIO_MUX_DETECT_SPINEL_MS},
    {115200, KIND_ZNP, RADIO_MUX_DETECT_ZNP_MS},
    {115200, KIND_ASH, RADIO_MUX_DETECT_EZSP_MS},
    {460800, KIND_ASH, RADIO_MUX_DETECT_EZSP_MS},
};
#define N_STEPS (sizeof(STEPS) / sizeof(STEPS[0]))

#define SPINEL_HEADER 0x81 // TID 1
#define SPINEL_CMD_GET 2
#define SPINEL_CMD_IS 6
#define SPINEL_PROP_LAST_STATUS 0
#define SPINEL_PROP_NCP_VERSION 2

static const uint8_t ZNP_PING[] = {0xFE, 0x00, 0x21, 0x01, 0x20};
static const uint8_t ZNP_VERSION[] = {0xFE, 0x00, 0x21, 0x02, 0x23};
static const uint8_t ASH_RST[] = {0x1A, 0xC0, 0x38, 0xBC, 0x7E};
#define ZNP_SOF 0xFE
#define ZNP_SRSP_SYS 0x61
#define ZNP_PING_ID 0x01
#define ZNP_VERSION_ID 0x02
#define ZNP_LEN_VERSION 5
#define ZNP_LEN_VERSION_REV 9
#define ZNP_LEN_MAX 9
#define ASH_FLAG 0x7E
#define ASH_ESC 0x7D
#define ASH_CANCEL 0x1A
#define ASH_SUBSTITUTE 0x18
#define ASH_XON 0x11
#define ASH_XOFF 0x13
#define ASH_RSTACK 0xC1
#define ASH_RSTACK_LEN 5

static size_t pack_uint(uint32_t v, uint8_t *out)
{
    size_t n = 0;

    while (v >= 0x80) {
        out[n++] = (uint8_t)(v | 0x80);
        v >>= 7;
    }
    out[n++] = (uint8_t)v;
    return n;
}

// Bytes consumed, 0 when the input ends inside the number or it doesn't fit 32 bits.
static size_t unpack_uint(const uint8_t *p, size_t n, uint32_t *v)
{
    uint32_t r = 0;

    for (size_t i = 0; i < n && i < 5; i++) {
        r |= (uint32_t)(p[i] & 0x7F) << (7 * i);
        if (!(p[i] & 0x80)) {
            *v = r;
            return i + 1;
        }
    }
    return 0;
}

static int64_t cap_end(const radio_mux_detect_t *d)
{
    return d->start_ms + RADIO_MUX_DETECT_CAP_MS;
}

static void tx_spinel_get(radio_mux_detect_t *d, uint32_t prop)
{
    uint8_t f[8];
    size_t n = 0;

    f[n++] = SPINEL_HEADER;
    f[n++] = SPINEL_CMD_GET;
    n += pack_uint(prop, f + n);
    d->tx_n = radio_mux_hdlc_encode(f, n, d->tx, sizeof(d->tx));
}

static void tx_literal(radio_mux_detect_t *d, const uint8_t *b, size_t n)
{
    memcpy(d->tx, b, n);
    d->tx_n = n;
}

static void enter_step(radio_mux_detect_t *d, int64_t now)
{
    const step_t *s = &STEPS[d->step];

    d->deadline_ms = now + s->budget_ms;
    d->acc_n = 0;
    d->ash_esc = false;
    switch (s->kind) {
    case KIND_SPINEL:
        d->stage = ST_SPINEL;
        radio_mux_hdlc_dec_init(&d->dec, d->dec_buf, sizeof(d->dec_buf));
        tx_spinel_get(d, SPINEL_PROP_NCP_VERSION);
        break;
    case KIND_ZNP:
        d->stage = ST_ZNP_PING;
        tx_literal(d, ZNP_PING, sizeof(ZNP_PING));
        break;
    default:
        d->stage = ST_ASH;
        tx_literal(d, ASH_RST, sizeof(ASH_RST));
        break;
    }
}

static void finish(radio_mux_detect_t *d, radio_mux_fw_t type)
{
    if (type == RADIO_MUX_FW_UNKNOWN) {
        memset(&d->rec, 0, sizeof(d->rec));
    } else {
        d->rec.baud = STEPS[d->step].baud;
    }
    d->rec.type = type;
    d->stage = ST_DONE;
}

void radio_mux_detect_start(radio_mux_detect_t *d, int64_t now_ms)
{
    memset(d, 0, sizeof(*d));
    d->stage = ST_START;
    d->start_ms = now_ms;
}

static const char *skip_space(const char *p, const char *end)
{
    while (p < end && (*p == ' ' || *p == '\t')) {
        p++;
    }
    return p;
}

// Copies [b, e) trimmed and cut to cap - 1 characters.
static void copy_trim(char *dst, size_t cap, const char *b, const char *e)
{
    size_t n;

    b = skip_space(b, e);
    while (e > b && (e[-1] == ' ' || e[-1] == '\t')) {
        e--;
    }
    n = (size_t)(e - b);
    if (cap == 0) {
        return;
    }
    if (n > cap - 1) {
        n = cap - 1;
    }
    memcpy(dst, b, n);
    dst[n] = '\0';
}

bool radio_mux_detect_parse_spinel_version(const char *s, char *version, size_t vcap, char *chip, size_t ccap)
{
    const char *end, *semi, *slash, *second;

    if (s == NULL || *s == '\0') {
        return false;
    }
    end = s + strlen(s);
    semi = memchr(s, ';', (size_t)(end - s));
    if (semi == NULL) {
        semi = end;
    }
    slash = memchr(s, '/', (size_t)(semi - s));
    copy_trim(version, vcap, slash ? slash + 1 : s, semi);
    chip[0] = '\0';
    if (semi < end) {
        second = semi + 1;
        end = memchr(second, ';', strlen(second));
        if (end == NULL) {
            end = second + strlen(second);
        }
        copy_trim(chip, ccap, second, end);
    }
    return true;
}

// ---- spinel

static void spinel_frame(radio_mux_detect_t *d, int64_t now, const uint8_t *f, size_t n)
{
    uint32_t cmd, prop;
    size_t used, at = 1;

    if (n < 3 || (f[0] & 0xC0) != 0x80) {
        return;
    }
    used = unpack_uint(f + at, n - at, &cmd);
    if (used == 0) {
        return;
    }
    at += used;
    used = unpack_uint(f + at, n - at, &prop);
    if (used == 0 || cmd != SPINEL_CMD_IS) {
        return;
    }
    at += used;
    if (d->stage == ST_SPINEL && prop == SPINEL_PROP_NCP_VERSION) {
        char s[RADIO_MUX_DETECT_RX_MAX];
        size_t len = 0;

        while (at + len < n && f[at + len] != '\0' && len < sizeof(s) - 1) {
            len++;
        }
        memcpy(s, f + at, len);
        s[len] = '\0';
        radio_mux_detect_parse_spinel_version(s, d->rec.version, sizeof(d->rec.version), d->rec.chip,
                                              sizeof(d->rec.chip));
        d->rec.type = RADIO_MUX_FW_SPINEL;
        d->stage = ST_VENDOR;
        d->deadline_ms = now + RADIO_MUX_DETECT_VENDOR_MS;
        tx_spinel_get(d, SPINEL_PROP_VENDOR_ZIGGURAT);
    } else if (d->stage == ST_VENDOR && prop == SPINEL_PROP_VENDOR_ZIGGURAT) {
        finish(d, RADIO_MUX_FW_ZIGGURAT);
    } else if (d->stage == ST_VENDOR && prop == SPINEL_PROP_LAST_STATUS) {
        finish(d, RADIO_MUX_FW_SPINEL);
    }
}

static void spinel_feed(radio_mux_detect_t *d, int64_t now, const uint8_t *rx, size_t n)
{
    int stage = d->stage;

    while (n > 0 && d->stage == stage) {
        bool done = false;
        size_t used = radio_mux_hdlc_decode_feed(&d->dec, rx, n, &done);

        rx += used;
        n -= used;
        if (done) {
            spinel_frame(d, now, d->dec_buf, d->dec.len);
            radio_mux_hdlc_dec_set_buf(&d->dec, d->dec_buf, sizeof(d->dec_buf));
        }
    }
}

// ---- ZNP

static const char *znp_chip(uint8_t product, char *buf, size_t cap)
{
    static const char *const CHIPS[] = {"CC2530", "CC2652", "CC1352"};

    if (product < sizeof(CHIPS) / sizeof(CHIPS[0])) {
        return CHIPS[product];
    }
    snprintf(buf, cap, "product %u", (unsigned)product);
    return buf;
}

static void znp_frame(radio_mux_detect_t *d, const uint8_t *f)
{
    uint8_t len = f[1];

    if (f[3] == ZNP_PING_ID && d->stage == ST_ZNP_PING) {
        d->stage = ST_ZNP_VER;
        tx_literal(d, ZNP_VERSION, sizeof(ZNP_VERSION));
    } else if (f[3] == ZNP_VERSION_ID && (len == ZNP_LEN_VERSION || len == ZNP_LEN_VERSION_REV)) {
        char tmp[sizeof(d->rec.chip)];
        const uint8_t *p = f + 4; // transport rev, product, major, minor, maint, [revision x4]
        int n = snprintf(d->rec.version, sizeof(d->rec.version), "%u.%u.%u", (unsigned)p[2], (unsigned)p[3],
                         (unsigned)p[4]);

        if (len == ZNP_LEN_VERSION_REV && n > 0) {
            uint32_t rev = (uint32_t)p[5] | (uint32_t)p[6] << 8 | (uint32_t)p[7] << 16 | (uint32_t)p[8] << 24;

            snprintf(d->rec.version + n, sizeof(d->rec.version) - (size_t)n, "-%lu", (unsigned long)rev);
        }
        snprintf(d->rec.chip, sizeof(d->rec.chip), "%s", znp_chip(p[1], tmp, sizeof(tmp)));
        finish(d, RADIO_MUX_FW_ZNP);
    }
}

static void drop_front(radio_mux_detect_t *d, size_t n)
{
    memmove(d->acc, d->acc + n, d->acc_n - n);
    d->acc_n -= n;
}

static void acc_push(radio_mux_detect_t *d, uint8_t b)
{
    if (d->acc_n == sizeof(d->acc)) {
        drop_front(d, 1);
    }
    d->acc[d->acc_n++] = b;
}

static void znp_feed(radio_mux_detect_t *d, const uint8_t *rx, size_t n)
{
    for (size_t i = 0; i < n && d->stage != ST_DONE; i++) {
        acc_push(d, rx[i]);
        // Scan: drop bytes until a start of frame that holds a valid SRSP; keep a partial frame for the next byte.
        for (;;) {
            size_t total;
            uint8_t fcs = 0;

            while (d->acc_n > 0 && d->acc[0] != ZNP_SOF) {
                drop_front(d, 1);
            }
            if (d->acc_n < 2) {
                break;
            }
            if (d->acc[1] > ZNP_LEN_MAX) {
                drop_front(d, 1);
                continue;
            }
            total = (size_t)d->acc[1] + 5;
            if (d->acc_n < total) {
                break;
            }
            for (size_t k = 1; k < total - 1; k++) {
                fcs ^= d->acc[k];
            }
            if (fcs == d->acc[total - 1] && d->acc[2] == ZNP_SRSP_SYS) {
                uint8_t frame[ZNP_LEN_MAX + 5];

                memcpy(frame, d->acc, total);
                drop_front(d, total);
                znp_frame(d, frame);
                break;
            }
            drop_front(d, 1);
        }
    }
}

// ---- ASH

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

// At a flag: the last five unstuffed bytes are the RSTACK (noise may precede it).
static void ash_flag(radio_mux_detect_t *d)
{
    const uint8_t *f;

    if (d->acc_n >= ASH_RSTACK_LEN) {
        f = d->acc + d->acc_n - ASH_RSTACK_LEN;
        if (f[0] == ASH_RSTACK && ash_crc(f, 3) == (uint16_t)(f[3] << 8 | f[4])) {
            snprintf(d->rec.version, sizeof(d->rec.version), "ash v%u", (unsigned)f[1]);
            finish(d, RADIO_MUX_FW_EZSP);
        }
    }
    d->acc_n = 0;
    d->ash_esc = false;
}

static void ash_feed(radio_mux_detect_t *d, const uint8_t *rx, size_t n)
{
    for (size_t i = 0; i < n && d->stage != ST_DONE; i++) {
        uint8_t b = rx[i];

        switch (b) {
        case ASH_FLAG:
            ash_flag(d);
            break;
        case ASH_CANCEL:
        case ASH_SUBSTITUTE:
            d->acc_n = 0;
            d->ash_esc = false;
            break;
        case ASH_XON:
        case ASH_XOFF:
            break;
        case ASH_ESC:
            d->ash_esc = true;
            break;
        default:
            if (d->ash_esc) {
                b ^= 0x20;
                d->ash_esc = false;
            }
            acc_push(d, b);
            break;
        }
    }
}

// ---- sequencer

radio_mux_probe_action_t radio_mux_detect_next(radio_mux_detect_t *d, int64_t now_ms, const uint8_t *rx, size_t rx_n,
                                               uint32_t *baud, const uint8_t **tx, size_t *tx_n,
                                               int64_t *wait_until_ms, radio_mux_fw_record_t *result)
{
    int64_t until;

    d->tx_n = 0;
    if (d->stage != ST_DONE && now_ms >= cap_end(d)) {
        // The cap wins over everything but a spinel firmware already seen.
        finish(d, d->stage == ST_VENDOR ? RADIO_MUX_FW_SPINEL : RADIO_MUX_FW_UNKNOWN);
    } else if (d->stage == ST_START) {
        enter_step(d, now_ms);
    } else if (d->stage != ST_DONE) {
        switch (d->stage) {
        case ST_SPINEL:
        case ST_VENDOR:
            spinel_feed(d, now_ms, rx, rx_n);
            break;
        case ST_ZNP_PING:
        case ST_ZNP_VER:
            znp_feed(d, rx, rx_n);
            break;
        default:
            ash_feed(d, rx, rx_n);
            break;
        }
        if (d->stage != ST_DONE && d->tx_n == 0 && now_ms >= d->deadline_ms) {
            if (d->stage == ST_VENDOR) {
                finish(d, RADIO_MUX_FW_SPINEL);
            } else if (d->stage == ST_ZNP_VER) {
                finish(d, RADIO_MUX_FW_ZNP);
            } else if (++d->step >= N_STEPS) {
                d->step = N_STEPS - 1;
                finish(d, RADIO_MUX_FW_UNKNOWN);
            } else {
                enter_step(d, now_ms);
            }
        }
    }

    if (result != NULL && d->stage == ST_DONE) {
        *result = d->rec;
    }
    *baud = STEPS[d->step].baud;
    *tx = d->tx_n > 0 ? d->tx : NULL;
    *tx_n = d->tx_n;
    until = d->deadline_ms < cap_end(d) ? d->deadline_ms : cap_end(d);
    *wait_until_ms = d->stage == ST_DONE ? now_ms : until;
    return d->stage == ST_DONE ? RADIO_MUX_PROBE_DONE : RADIO_MUX_PROBE_SEND;
}
