#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"
#include "unity_test_runner.h"
#include "usbip_engine.h"

#define W(n) ((n) & 0xff), (((n) >> 8) & 0xff)
#define DEVID 0x10003
// Two max reads plus this remainder fill the shared IN budget exactly: three transfers, so the bytes (not the
// 32-transfer count) are the limit that bites.
#define SHARED_REST (USBIP_INFLIGHT_BYTES - 2 * USBIP_XFER_MAX)
// Small enough that 1 MB of waiting data takes more than 60 of them (a max transfer would take only nine).
#define OVERLOAD_CHUNK (USBIP_XFER_MAX / 8)

// --- Fake ops: records every call in one log, keeps submitted transfers until the test completes them. ---

#define LOG_MAX 512
#define PEND_MAX 128
#define REP_MAX 256

typedef struct {
    uint32_t cmd, seq;
    int32_t status, actual;
    uint8_t data[64];
    size_t n;
} rep_t;

static struct {
    char log[LOG_MAX][32];
    int n_log;
    usbip_xfer_t *pend[PEND_MAX];
    int n_pend;
    rep_t rep[REP_MAX];
    int n_rep;
    int allocs, frees, dma_allocs;
    esp_err_t submit_ret;
    int fail_iface, fail_alt;
    bool fail_dma;  // DMA allocations fail  // claims of interface fail_iface - 1 at fail_alt fail; 0: none fail
    int64_t now;
    char why[64];
    int ends;
} f;

static usbip_engine_t eng;

// Ends a session a failed test may have left running, and gives back what the fake holds.
static void finish_quiet(void)
{
    usbip_engine_stop(&eng, "test end");
    while (f.n_pend > 0) {
        usbip_xfer_t *x = f.pend[--f.n_pend];

        usbip_engine_on_complete(&eng, x, USBIP_XS_CANCELED, 0);
    }
}

static void logf_(const char *fmt, unsigned a, unsigned b)
{
    TEST_ASSERT_LESS_THAN(LOG_MAX, f.n_log);
    snprintf(f.log[f.n_log++], sizeof(f.log[0]), fmt, a, b);
}

static esp_err_t fake_submit(void *ctx, usbip_xfer_t *x)
{
    logf_("submit %u", x->seqnum, 0);
    if (f.submit_ret != ESP_OK) {
        return f.submit_ret;
    }
    TEST_ASSERT_LESS_THAN(PEND_MAX, f.n_pend);
    f.pend[f.n_pend++] = x;
    return ESP_OK;
}

static esp_err_t fake_halt(void *ctx, uint8_t ep)
{
    TEST_ASSERT_NOT_EQUAL(0, ep);
    logf_("halt %02x", ep, 0);
    return ESP_OK;
}

static esp_err_t fake_flush(void *ctx, uint8_t ep)
{
    TEST_ASSERT_NOT_EQUAL(0, ep);
    logf_("flush %02x", ep, 0);
    return ESP_OK;
}

static esp_err_t fake_clear(void *ctx, uint8_t ep)
{
    TEST_ASSERT_NOT_EQUAL(0, ep);
    logf_("clear %02x", ep, 0);
    return ESP_OK;
}

static esp_err_t fake_claim(void *ctx, uint8_t iface, uint8_t alt)
{
    logf_("claim %u %u", iface, alt);
    return f.fail_iface == iface + 1 && f.fail_alt == alt ? ESP_FAIL : ESP_OK;
}

static esp_err_t fake_release(void *ctx, uint8_t iface)
{
    logf_("release %u", iface, 0);
    return ESP_OK;
}

static void fake_power(void *ctx)
{
    logf_("power", 0, 0);
}

static void *fake_alloc(void *ctx, size_t n, bool dma)
{
    if (dma && f.fail_dma) {
        return NULL;
    }
    f.allocs++;
    f.dma_allocs += dma;
    return calloc(1, n ? n : 1);
}

static void fake_free(void *ctx, void *p)
{
    if (p != NULL) {
        f.frees++;
    }
    free(p);
}

static uint32_t get32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void fake_emit(void *ctx, const uint8_t hdr[USBIP_ENGINE_HDR_LEN], const uint8_t *data, size_t n)
{
    TEST_ASSERT_LESS_THAN(REP_MAX, f.n_rep);
    rep_t *r = &f.rep[f.n_rep++];

    r->cmd = get32(hdr);
    r->seq = get32(hdr + 4);
    r->status = (int32_t)get32(hdr + 20);
    r->actual = r->cmd == USBIP_RET_SUBMIT ? (int32_t)get32(hdr + 24) : 0;
    r->n = n;
    if (n > 0) {
        memcpy(r->data, data, n < sizeof(r->data) ? n : sizeof(r->data));
    }
    logf_(r->cmd == USBIP_RET_SUBMIT ? "ret_submit %u" : "ret_unlink %u", r->seq, 0);
}

static void fake_end(void *ctx, const char *why)
{
    f.ends++;
    snprintf(f.why, sizeof(f.why), "%s", why);
    logf_("end", 0, 0);
}

static int64_t fake_now(void *ctx)
{
    return f.now;
}

static const usbip_engine_ops_t ops = {
    .submit = fake_submit,
    .ep_halt = fake_halt,
    .ep_flush = fake_flush,
    .ep_clear = fake_clear,
    .iface_claim = fake_claim,
    .iface_release = fake_release,
    .port_power_cycle = fake_power,
    .alloc = fake_alloc,
    .free = fake_free,
    .emit = fake_emit,
    .end_session = fake_end,
    .now_ms = fake_now,
};

// --- Configurations ---

// CP2102N: one vendor interface, bulk IN 0x81 and bulk OUT 0x01.
static const uint8_t cfg_cp[] = {
    9, 2, W(32), 1, 1, 0, 0x80, 50,
    9, 4, 0, 0, 2, 0xff, 0, 0, 0,
    7, 5, 0x81, 2, W(64), 0,
    7, 5, 0x01, 2, W(64), 0,
};

// Bluetooth adapter: interface 0 has intr IN 0x81, bulk IN 0x82, bulk OUT 0x02; interface 1 iso only.
static const uint8_t cfg_bt[] = {
    9, 2, W(85), 2, 1, 0, 0xe0, 50,
    9, 4, 0, 0, 3, 0xe0, 1, 1, 0,
    7, 5, 0x81, 3, W(16), 1,
    7, 5, 0x82, 2, W(64), 0,
    7, 5, 0x02, 2, W(64), 0,
    9, 4, 1, 0, 2, 0xe0, 1, 1, 0,
    7, 5, 0x83, 1, W(0), 1,
    7, 5, 0x03, 1, W(0), 1,
    9, 4, 1, 1, 2, 0xe0, 1, 1, 0,
    7, 5, 0x83, 1, W(9), 1,
    7, 5, 0x03, 1, W(9), 1,
};

// Interface 0: alt 0 bulk 0x81/0x01, alt 1 adds intr IN 0x83. Interface 1: bulk IN 0x82.
static const uint8_t cfg_alt[] = {
    9, 2, W(78), 2, 1, 0, 0x80, 50,
    9, 4, 0, 0, 2, 0xff, 0, 0, 0,
    7, 5, 0x81, 2, W(64), 0,
    7, 5, 0x01, 2, W(64), 0,
    9, 4, 0, 1, 3, 0xff, 0, 0, 0,
    7, 5, 0x81, 2, W(64), 0,
    7, 5, 0x01, 2, W(64), 0,
    7, 5, 0x83, 3, W(8), 1,
    9, 4, 1, 0, 1, 0xff, 0, 0, 0,
    7, 5, 0x82, 2, W(64), 0,
};

static void start(const uint8_t *cfg, size_t len)
{
    finish_quiet();
    memset(&f, 0, sizeof(f));
    memset(&eng, 0, sizeof(eng));  // a fresh boot: counters and peaks from 0
    usbip_dev_rec_t dev = {.busnum = 1, .devnum = 3, .config_value = 1, .num_ifaces = cfg[4]};

    TEST_ASSERT_EQUAL(ESP_OK, usbip_engine_start(&eng, &ops, NULL, &dev, cfg, len));
    f.n_log = 0;
}

// Stops the engine and completes whatever the fake still holds as canceled, so no test leaks.
static void finish(void)
{
    finish_quiet();
    TEST_ASSERT_EQUAL_INT(f.allocs, f.frees);
}

// --- Commands and completions ---

static void submit_full(uint32_t seq, uint32_t devid, uint8_t ep, bool in, int32_t len, uint32_t flags,
                        const uint8_t *setup, const uint8_t *out)
{
    usbip_cmd_t c = {
        .command = USBIP_CMD_SUBMIT, .seqnum = seq, .devid = devid, .direction = in ? USBIP_DIR_IN : USBIP_DIR_OUT,
        .ep = ep & 0x0f,
    };

    c.submit.flags = flags;
    c.submit.length = len;
    if (setup != NULL) {
        memcpy(c.submit.setup, setup, 8);
    }
    usbip_engine_on_cmd(&eng, &c, out);
}

static uint8_t out_buf[USBIP_XFER_MAX];

static void submit(uint32_t seq, uint8_t ep, int32_t len)
{
    bool in = ep & 0x80;

    submit_full(seq, DEVID, ep, in, len, 0, NULL, in ? NULL : out_buf);
}

static void control(uint32_t seq, uint8_t type, uint8_t req, uint16_t value, uint16_t index, uint16_t len)
{
    const uint8_t setup[8] = {type, req, W(value), W(index), W(len)};
    bool in = type & 0x80;

    submit_full(seq, DEVID, 0, in, len, 0, setup, in ? NULL : out_buf);
}

static void unlink(uint32_t seq, uint32_t target)
{
    usbip_cmd_t c = {.command = USBIP_CMD_UNLINK, .seqnum = seq, .devid = DEVID};

    c.unlink.unlink_seqnum = target;
    usbip_engine_on_cmd(&eng, &c, NULL);
}

static usbip_xfer_t *pending(uint32_t seq)
{
    for (int i = 0; i < f.n_pend; i++) {
        if (f.pend[i]->seqnum == seq) {
            return f.pend[i];
        }
    }
    return NULL;
}

// Completes the pending transfer seq; an IN completion carries bytes 0, 1, 2, ... as data.
static void complete(uint32_t seq, usbip_xfer_status_t st, size_t actual)
{
    for (int i = 0; i < f.n_pend; i++) {
        usbip_xfer_t *x = f.pend[i];

        if (x->seqnum != seq) {
            continue;
        }
        memmove(&f.pend[i], &f.pend[i + 1], (size_t)(f.n_pend - i - 1) * sizeof(f.pend[0]));
        f.n_pend--;
        if (x->in) {
            size_t off = x->ep == 0 ? 8 : 0;

            for (size_t k = off; k < actual && k < x->len; k++) {
                x->buf[k] = (uint8_t)(k - off);
            }
        }
        usbip_engine_on_complete(&eng, x, st, actual);
        return;
    }
    TEST_FAIL_MESSAGE("transfer not pending");
}

static int log_find_from(int from, const char *s)
{
    for (int i = from; i < f.n_log; i++) {
        if (strcmp(f.log[i], s) == 0) {
            return i;
        }
    }
    return -1;
}

static int log_find(const char *s)
{
    return log_find_from(0, s);
}

// Counts log lines that are prefix or start with prefix and a space ("submit" counts "submit 1", "submit 1" does not
// count "submit 10").
static int log_count(const char *prefix)
{
    size_t k = strlen(prefix);
    int n = 0;

    for (int i = 0; i < f.n_log; i++) {
        n += strncmp(f.log[i], prefix, k) == 0 && (f.log[i][k] == '\0' || f.log[i][k] == ' ');
    }
    return n;
}

// Asserts that the given log lines appear in this order (each searched after the previous one).
static void assert_order(const char *const *lines, int n)
{
    int prev = -1;

    for (int i = 0; i < n; i++) {
        int at = log_find_from(prev + 1, lines[i]);

        if (at < 0) {
            TEST_FAIL_MESSAGE(lines[i]);
        }
        prev = at;
    }
}

#define ASSERT_ORDER(...) do { \
        const char *const l_[] = {__VA_ARGS__}; \
        assert_order(l_, sizeof(l_) / sizeof(l_[0])); \
    } while (0)

static const rep_t *reply(uint32_t cmd, uint32_t seq)
{
    for (int i = 0; i < f.n_rep; i++) {
        if (f.rep[i].cmd == cmd && f.rep[i].seq == seq) {
            return &f.rep[i];
        }
    }
    return NULL;
}

static int replies_for(uint32_t cmd, uint32_t seq)
{
    int n = 0;

    for (int i = 0; i < f.n_rep; i++) {
        n += f.rep[i].cmd == cmd && f.rep[i].seq == seq;
    }
    return n;
}

static void assert_ret_submit(uint32_t seq, int32_t status, int32_t actual)
{
    const rep_t *r = reply(USBIP_RET_SUBMIT, seq);

    TEST_ASSERT_NOT_NULL_MESSAGE(r, "no RET_SUBMIT");
    TEST_ASSERT_EQUAL_INT32(status, r->status);
    TEST_ASSERT_EQUAL_INT32(actual, r->actual);
}

static void assert_ret_unlink(uint32_t seq, int32_t status)
{
    const rep_t *r = reply(USBIP_RET_UNLINK, seq);

    TEST_ASSERT_NOT_NULL_MESSAGE(r, "no RET_UNLINK");
    TEST_ASSERT_EQUAL_INT32(status, r->status);
}

// --- CMD_SUBMIT ---

TEST_CASE("bulk or interrupt transfer within budget: submits it on the endpoint in arrival order", "[usbip]")
{
    start(cfg_cp, sizeof(cfg_cp));
    submit(1, 0x81, 64);
    submit(2, 0x81, 100);
    submit(3, 0x81, 64);
    submit(4, 0x01, 10);
    ASSERT_ORDER("submit 1", "submit 2", "submit 3", "submit 4");
    TEST_ASSERT_EQUAL_INT(4, f.n_pend);
    usbip_xfer_t *x = pending(2);

    TEST_ASSERT_EQUAL_HEX8(0x81, x->ep);
    TEST_ASSERT_TRUE(x->in);
    TEST_ASSERT_EQUAL_UINT8(2, x->type);
    TEST_ASSERT_EQUAL_size_t(128, x->len);  // rounded up to the MPS
    x = pending(4);
    TEST_ASSERT_EQUAL_HEX8(0x01, x->ep);
    TEST_ASSERT_FALSE(x->in);
    TEST_ASSERT_EQUAL_size_t(10, x->len);
    finish();
}

TEST_CASE("bulk or interrupt transfer within budget: answers RET_SUBMIT with the mapped status, actual_length and IN data",
          "[usbip]")
{
    start(cfg_cp, sizeof(cfg_cp));
    submit(1, 0x81, 64);
    for (int i = 0; i < 10; i++) {
        out_buf[i] = (uint8_t)(0xa0 + i);
    }
    submit(2, 0x01, 10);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(out_buf, pending(2)->buf, 10);
    complete(1, USBIP_XS_COMPLETED, 5);
    complete(2, USBIP_XS_COMPLETED, 10);
    assert_ret_submit(1, 0, 5);
    const uint8_t want[5] = {0, 1, 2, 3, 4};

    TEST_ASSERT_EQUAL_size_t(5, reply(USBIP_RET_SUBMIT, 1)->n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, reply(USBIP_RET_SUBMIT, 1)->data, 5);
    assert_ret_submit(2, 0, 10);
    TEST_ASSERT_EQUAL_size_t(0, reply(USBIP_RET_SUBMIT, 2)->n);
    usbip_counters_t c;

    usbip_engine_counters(&eng, &c);
    TEST_ASSERT_EQUAL_UINT32(2, c.submits);
    TEST_ASSERT_EQUAL_UINT32(2, c.completed);
    TEST_ASSERT_EQUAL_UINT32(0, c.errors);
    TEST_ASSERT_EQUAL_UINT64(5, c.bytes_in);
    TEST_ASSERT_EQUAL_UINT64(10, c.bytes_out);
    finish();
}

TEST_CASE("OUT endpoint with a transfer in flight: queues it in the endpoint's FIFO until the first completes", "[usbip]")
{
    start(cfg_cp, sizeof(cfg_cp));
    submit(1, 0x01, 64);
    submit(2, 0x01, 64);
    TEST_ASSERT_EQUAL_INT(1, log_count("submit"));
    TEST_ASSERT_NULL(pending(2));
    complete(1, USBIP_XS_COMPLETED, 64);
    ASSERT_ORDER("submit 1", "ret_submit 1", "submit 2");
    TEST_ASSERT_NOT_NULL(pending(2));
    finish();
}

TEST_CASE("STALL or error completion: answers its RET_SUBMIT, then clears the endpoint so its queue continues",
          "[usbip]")
{
    start(cfg_cp, sizeof(cfg_cp));
    submit(1, 0x81, 64);
    submit(2, 0x81, 64);
    submit(3, 0x81, 64);
    complete(1, USBIP_XS_STALL, 0);
    assert_ret_submit(1, -USBIP_EPIPE, 0);
    ASSERT_ORDER("ret_submit 1", "clear 81");
    TEST_ASSERT_EQUAL_INT(-1, log_find("halt 81"));
    TEST_ASSERT_EQUAL_INT(-1, log_find("flush 81"));
    TEST_ASSERT_NOT_NULL(pending(2));  // the rest of the queue is untouched
    complete(2, USBIP_XS_ERROR, 0);
    assert_ret_submit(2, -USBIP_EPROTO, 0);
    TEST_ASSERT_EQUAL_INT(2, log_count("clear 81"));
    complete(3, USBIP_XS_COMPLETED, 64);
    assert_ret_submit(3, 0, 64);
    usbip_counters_t c;

    usbip_engine_counters(&eng, &c);
    TEST_ASSERT_EQUAL_UINT32(2, c.errors);
    TEST_ASSERT_EQUAL_UINT32(1, c.completed);
    finish();
}

TEST_CASE("short IN with URB_SHORT_NOT_OK: answers -EREMOTEIO", "[usbip]")
{
    start(cfg_cp, sizeof(cfg_cp));
    submit_full(1, DEVID, 0x81, true, 64, USBIP_URB_SHORT_NOT_OK, NULL, NULL);
    complete(1, USBIP_XS_COMPLETED, 3);
    assert_ret_submit(1, -USBIP_EREMOTEIO, 3);
    finish();
}

TEST_CASE("transfer above USBIP_XFER_MAX, an iso endpoint, or an endpoint not claimed: answers -EINVAL at once", "[usbip]")
{
    start(cfg_bt, sizeof(cfg_bt));
    submit(1, 0x82, USBIP_XFER_MAX + 1);
    submit(2, 0x83, 9);   // iso
    submit(3, 0x84, 64);  // not in the configuration
    submit(4, 0x01, 64);  // wrong direction of a claimed endpoint number
    control(5, 0x80, 6, 0x0200, 0, USBIP_CTRL_DATA_MAX + 1);
    for (uint32_t s = 1; s <= 5; s++) {
        assert_ret_submit(s, -USBIP_EINVAL, 0);
    }
    TEST_ASSERT_EQUAL_INT(0, log_count("submit"));
    finish();
}

TEST_CASE("wrong devid: answers -ENODEV", "[usbip]")
{
    start(cfg_cp, sizeof(cfg_cp));
    submit_full(1, 0x10004, 0x81, true, 64, 0, NULL, NULL);
    assert_ret_submit(1, -USBIP_ENODEV, 0);
    TEST_ASSERT_EQUAL_INT(0, log_count("submit"));
    finish();
}

TEST_CASE("budget full: queues it in the endpoint's PSRAM FIFO and submits it in order when a slot frees", "[usbip]")
{
    start(cfg_cp, sizeof(cfg_cp));
    submit(1, 0x81, USBIP_XFER_MAX);
    submit(2, 0x81, USBIP_XFER_MAX);
    submit(3, 0x81, SHARED_REST);  // shared 256 KB full
    int dma = f.dma_allocs;

    submit(4, 0x81, 64);
    submit(5, 0x81, 64);
    TEST_ASSERT_EQUAL_INT(3, log_count("submit"));
    TEST_ASSERT_EQUAL_INT(dma, f.dma_allocs);  // waiting transfers hold no DMA memory
    usbip_mem_stats_t m;

    usbip_engine_mem(&eng, &m);
    TEST_ASSERT_EQUAL_size_t(USBIP_INFLIGHT_BYTES, m.inflight);
    TEST_ASSERT_GREATER_THAN(0, m.waiting);
    complete(1, USBIP_XS_COMPLETED, 0);
    ASSERT_ORDER("ret_submit 1", "submit 4", "submit 5");
    usbip_engine_mem(&eng, &m);
    TEST_ASSERT_EQUAL_size_t(0, m.waiting);
    TEST_ASSERT_EQUAL_size_t(USBIP_XFER_MAX + SHARED_REST + 128, m.inflight);
    finish();
}

TEST_CASE("endpoint with nothing in flight: grants it a slot even when the shared budget is used up", "[usbip]")
{
    start(cfg_bt, sizeof(cfg_bt));
    submit(1, 0x82, USBIP_XFER_MAX);
    submit(2, 0x82, USBIP_XFER_MAX);
    submit(3, 0x82, SHARED_REST);
    submit(4, 0x82, 64);  // waits: shared budget used up
    submit(5, 0x81, 16);  // reserve of 0x81
    TEST_ASSERT_NULL(pending(4));
    TEST_ASSERT_NOT_NULL(pending(5));
    finish();
}

TEST_CASE("budget pressure: a large first OUT over the hard cap waits and goes out when a large transfer frees",
          "[usbip]")
{
    start(cfg_bt, sizeof(cfg_bt));
    submit(1, 0x81, USBIP_XFER_MAX);  // IN shared: 256 KB full by bytes, 3 transfers
    submit(2, 0x81, USBIP_XFER_MAX);
    submit(3, 0x81, SHARED_REST);
    submit(40, 0x82, USBIP_XFER_MAX);  // large reserve of 0x82: charged 404 KB
    submit(41, 0x02, USBIP_XFER_MAX);  // would be 522 KB: waits
    TEST_ASSERT_NOT_NULL(pending(40));
    TEST_ASSERT_NULL(pending(41));
    control(42, 0x20, 0, 0, 0, 3);  // EP0's small reserve still goes
    TEST_ASSERT_NOT_NULL(pending(42));
    complete(40, USBIP_XS_COMPLETED, 0);
    TEST_ASSERT_NOT_NULL(pending(41));
    usbip_mem_stats_t m;

    usbip_engine_mem(&eng, &m);
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(USBIP_INFLIGHT_HARD, m.inflight_peak);
    finish();
}

TEST_CASE("Budget full of IN reads on 0x81: a bulk OUT on 0x02 is still submitted", "[usbip]")
{
    start(cfg_bt, sizeof(cfg_bt));
    // btusb keeps interrupt and bulk reads pending; here small reads on 0x81 hit the 32-transfer limit (not the bytes).
    for (uint32_t s = 1; s <= 34; s++) {
        submit(s, 0x81, 1024);
    }
    TEST_ASSERT_EQUAL_INT(USBIP_INFLIGHT_XFERS, log_count("submit"));
    submit(40, 0x02, 300);                     // ACL data out
    control(41, 0x20, 0, 0, 0, 3);             // HCI command on EP0
    TEST_ASSERT_NOT_NULL(pending(40));
    TEST_ASSERT_NOT_NULL(pending(41));
    finish();
}

TEST_CASE("waiting data exceeds 1 MB: ends the session and counts sessions_ended_overload", "[usbip]")
{
    start(cfg_cp, sizeof(cfg_cp));
    submit(1, 0x01, OVERLOAD_CHUNK);  // in flight
    uint32_t s = 2;

    for (; s < 200 && f.ends == 0; s++) {
        submit(s, 0x01, OVERLOAD_CHUNK);
    }
    TEST_ASSERT_EQUAL_INT(1, f.ends);
    TEST_ASSERT_GREATER_THAN(60, s);
    TEST_ASSERT_FALSE(usbip_engine_active(&eng));
    usbip_counters_t c;

    usbip_engine_counters(&eng, &c);
    TEST_ASSERT_EQUAL_UINT32(1, c.overload_ends);
    usbip_mem_stats_t m;

    usbip_engine_mem(&eng, &m);
    TEST_ASSERT_EQUAL_size_t(0, m.waiting);
    TEST_ASSERT_GREATER_THAN(USBIP_WAIT_BYTES - OVERLOAD_CHUNK - 1024, m.waiting_peak);
    TEST_ASSERT_EQUAL_INT(1, log_count("power"));
    finish();
}

TEST_CASE("submit refused by the USB side: answers -EPROTO and frees the transfer", "[usbip]")
{
    start(cfg_cp, sizeof(cfg_cp));
    f.submit_ret = ESP_ERR_INVALID_STATE;
    submit(1, 0x81, 64);
    assert_ret_submit(1, -USBIP_EPROTO, 0);
    usbip_mem_stats_t m;

    usbip_engine_mem(&eng, &m);
    TEST_ASSERT_EQUAL_size_t(0, m.inflight);
    TEST_ASSERT_EQUAL_INT(f.allocs, f.frees);
    f.submit_ret = ESP_OK;
    finish();
}

// --- Standard requests on EP0 ---

TEST_CASE("SET_CONFIGURATION to the active value: answers 0 without sending anything to the device", "[usbip]")
{
    start(cfg_cp, sizeof(cfg_cp));
    control(1, 0x00, 9, 1, 0, 0);
    assert_ret_submit(1, 0, 0);
    TEST_ASSERT_EQUAL_INT(0, log_count("submit"));
    finish();
}

TEST_CASE("SET_CONFIGURATION to another value: answers -EPIPE", "[usbip]")
{
    start(cfg_cp, sizeof(cfg_cp));
    control(1, 0x00, 9, 2, 0, 0);
    assert_ret_submit(1, -USBIP_EPIPE, 0);
    TEST_ASSERT_EQUAL_INT(0, log_count("submit"));
    finish();
}

TEST_CASE("SET_INTERFACE: cancels the interface's pending transfers with -ECONNRESET", "[usbip]")
{
    start(cfg_alt, sizeof(cfg_alt));
    submit(1, 0x81, 64);
    submit(2, 0x81, 64);
    submit(3, 0x01, 64);
    submit(4, 0x01, 64);  // waits behind 3
    submit(5, 0x82, 64);  // interface 1: untouched
    control(10, 0x01, 11, 1, 0, 0);
    assert_ret_submit(4, -USBIP_ECONNRESET, 0);  // waiting: at once
    complete(1, USBIP_XS_CANCELED, 0);
    complete(2, USBIP_XS_CANCELED, 0);
    complete(3, USBIP_XS_CANCELED, 0);
    assert_ret_submit(1, -USBIP_ECONNRESET, 0);
    assert_ret_submit(2, -USBIP_ECONNRESET, 0);
    assert_ret_submit(3, -USBIP_ECONNRESET, 0);
    TEST_ASSERT_EQUAL_INT(1, log_count("submit 1"));  // canceled, not resubmitted
    TEST_ASSERT_EQUAL_INT(-1, log_find("halt 82"));
    TEST_ASSERT_NOT_NULL(pending(5));
    finish();
}

TEST_CASE("SET_INTERFACE: forwards the request and claims the interface at the new alt", "[usbip]")
{
    start(cfg_alt, sizeof(cfg_alt));
    submit(1, 0x81, 64);
    control(10, 0x01, 11, 1, 0, 0);
    ASSERT_ORDER("halt 81", "flush 81", "clear 81", "release 0", "submit 10");
    TEST_ASSERT_EQUAL_INT(-1, log_find("claim 0 1"));
    const uint8_t want_setup[8] = {0x01, 11, 1, 0, 0, 0, 0, 0};

    TEST_ASSERT_EQUAL_HEX8_ARRAY(want_setup, pending(10)->buf, 8);
    complete(10, USBIP_XS_COMPLETED, 8);
    ASSERT_ORDER("submit 10", "claim 0 1", "ret_submit 10");
    assert_ret_submit(10, 0, 0);
    submit(11, 0x83, 8);  // only on alt 1
    TEST_ASSERT_NOT_NULL(pending(11));
    finish();
}

TEST_CASE("SET_INTERFACE: on failure, claims it at the previous alt again and answers the failure", "[usbip]")
{
    start(cfg_alt, sizeof(cfg_alt));
    control(10, 0x01, 11, 1, 0, 0);
    complete(10, USBIP_XS_STALL, 8);
    ASSERT_ORDER("release 0", "submit 10", "claim 0 0", "ret_submit 10");
    TEST_ASSERT_EQUAL_INT(-1, log_find("claim 0 1"));
    assert_ret_submit(10, -USBIP_EPIPE, 0);
    submit(11, 0x83, 8);
    assert_ret_submit(11, -USBIP_EINVAL, 0);
    submit(12, 0x81, 64);
    TEST_ASSERT_NOT_NULL(pending(12));
    finish();
}

TEST_CASE("SET_INTERFACE: when the new alt cannot be claimed, claims the previous alt and answers -EPROTO", "[usbip]")
{
    start(cfg_alt, sizeof(cfg_alt));
    f.fail_iface = 0 + 1;
    f.fail_alt = 1;
    control(10, 0x01, 11, 1, 0, 0);
    complete(10, USBIP_XS_COMPLETED, 8);
    ASSERT_ORDER("submit 10", "claim 0 1", "claim 0 0", "ret_submit 10");
    assert_ret_submit(10, -USBIP_EPROTO, 0);
    submit(11, 0x81, 64);
    TEST_ASSERT_NOT_NULL(pending(11));
    f.fail_iface = 0;
    finish();
}

TEST_CASE("CLEAR_FEATURE(ENDPOINT_HALT): forwards it to the device", "[usbip]")
{
    start(cfg_cp, sizeof(cfg_cp));
    control(10, 0x02, 1, 0, 0x81, 0);
    usbip_xfer_t *x = pending(10);

    TEST_ASSERT_NOT_NULL(x);
    const uint8_t want_setup[8] = {0x02, 1, 0, 0, 0x81, 0, 0, 0};

    TEST_ASSERT_EQUAL_HEX8(0, x->ep);
    TEST_ASSERT_EQUAL_size_t(8, x->len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want_setup, x->buf, 8);
    complete(10, USBIP_XS_COMPLETED, 8);
    assert_ret_submit(10, 0, 0);
    finish();
}

TEST_CASE("CLEAR_FEATURE(ENDPOINT_HALT): answers -ECONNRESET for transfers pending on that endpoint", "[usbip]")
{
    start(cfg_cp, sizeof(cfg_cp));
    submit(1, 0x81, 64);
    submit(2, 0x81, 64);
    control(10, 0x02, 1, 0, 0x81, 0);
    complete(10, USBIP_XS_COMPLETED, 8);
    complete(1, USBIP_XS_CANCELED, 0);
    complete(2, USBIP_XS_CANCELED, 0);
    assert_ret_submit(1, -USBIP_ECONNRESET, 0);
    assert_ret_submit(2, -USBIP_ECONNRESET, 0);
    TEST_ASSERT_EQUAL_INT(1, log_count("submit 1"));
    TEST_ASSERT_EQUAL_INT(1, log_count("submit 2"));
    finish();
}

TEST_CASE("CLEAR_FEATURE(ENDPOINT_HALT): re-claims the interface at its current alt and resubmits the other endpoints' "
          "transfers in order", "[usbip]")
{
    start(cfg_bt, sizeof(cfg_bt));
    submit(1, 0x82, 64);
    submit(2, 0x81, 16);
    submit(3, 0x81, 16);
    submit(5, 0x81, 16);
    control(10, 0x02, 1, 0, 0x82, 0);
    complete(10, USBIP_XS_COMPLETED, 8);
    ASSERT_ORDER("halt 81", "flush 81", "clear 81", "halt 82", "flush 82", "clear 82", "release 0", "claim 0 0",
                 "ret_submit 10");
    TEST_ASSERT_EQUAL_INT(-1, log_find("release 1"));
    TEST_ASSERT_EQUAL_INT(-1, log_find("halt 02"));  // nothing in flight there
    f.n_log = 0;
    complete(3, USBIP_XS_CANCELED, 0);
    complete(1, USBIP_XS_CANCELED, 0);
    complete(5, USBIP_XS_CANCELED, 0);
    complete(2, USBIP_XS_CANCELED, 0);
    assert_ret_submit(1, -USBIP_ECONNRESET, 0);
    ASSERT_ORDER("submit 2", "submit 3", "submit 5");
    TEST_ASSERT_NULL(reply(USBIP_RET_SUBMIT, 2));
    TEST_ASSERT_NULL(reply(USBIP_RET_SUBMIT, 3));
    TEST_ASSERT_NULL(reply(USBIP_RET_SUBMIT, 5));
    TEST_ASSERT_NOT_NULL(pending(2));
    TEST_ASSERT_NOT_NULL(pending(3));
    TEST_ASSERT_NOT_NULL(pending(5));
    finish();
}

TEST_CASE("CLEAR_FEATURE(ENDPOINT_HALT): a canceled OUT on another endpoint answers -ECONNRESET and is never "
          "resubmitted", "[usbip]")
{
    start(cfg_bt, sizeof(cfg_bt));
    submit(1, 0x82, 64);
    submit(4, 0x02, 64);
    control(10, 0x02, 1, 0, 0x82, 0);
    complete(10, USBIP_XS_COMPLETED, 8);
    ASSERT_ORDER("halt 02", "flush 02", "clear 02", "release 0", "claim 0 0");
    complete(4, USBIP_XS_CANCELED, 0);
    complete(1, USBIP_XS_CANCELED, 0);
    assert_ret_submit(4, -USBIP_ECONNRESET, 0);
    TEST_ASSERT_EQUAL_INT(1, log_count("submit 4"));
    TEST_ASSERT_NULL(pending(4));
    finish();
}

TEST_CASE("CLEAR_FEATURE(ENDPOINT_HALT): when the interface cannot be claimed again, its transfers answer -ECONNRESET "
          "and its endpoints -EINVAL", "[usbip]")
{
    start(cfg_cp, sizeof(cfg_cp));
    submit(1, 0x81, 64);  // collateral of a clear on 0x01
    f.fail_iface = 0 + 1;
    f.fail_alt = 0;
    control(10, 0x02, 1, 0, 0x01, 0);
    complete(10, USBIP_XS_COMPLETED, 8);
    ASSERT_ORDER("halt 81", "flush 81", "clear 81", "release 0", "claim 0 0", "ret_submit 10");
    assert_ret_submit(10, 0, 0);  // the device cleared its halt
    complete(1, USBIP_XS_CANCELED, 0);
    assert_ret_submit(1, -USBIP_ECONNRESET, 0);
    TEST_ASSERT_EQUAL_INT(1, log_count("submit 1"));
    submit(2, 0x81, 64);
    assert_ret_submit(2, -USBIP_EINVAL, 0);
    f.fail_iface = 0;
    f.n_log = 0;
    finish();
    TEST_ASSERT_EQUAL_INT(0, log_count("release 0"));  // not claimed any more
}

TEST_CASE("SET_ADDRESS: answers -EPIPE", "[usbip]")
{
    start(cfg_cp, sizeof(cfg_cp));
    control(1, 0x00, 5, 7, 0, 0);
    assert_ret_submit(1, -USBIP_EPIPE, 0);
    TEST_ASSERT_EQUAL_INT(0, log_count("submit"));
    finish();
}

TEST_CASE("port reset: answers 0 and resets nothing", "[usbip]")
{
    start(cfg_cp, sizeof(cfg_cp));
    control(1, 0x23, 3, 4, 1, 0);
    assert_ret_submit(1, 0, 0);
    TEST_ASSERT_EQUAL_INT(1, f.n_log);  // only the reply
    finish();
}

TEST_CASE("any other request with up to 4096 data bytes: forwards it and answers its status and data", "[usbip]")
{
    start(cfg_cp, sizeof(cfg_cp));
    control(1, 0x80, 6, 0x0100, 0, 18);
    usbip_xfer_t *x = pending(1);
    const uint8_t want_setup[8] = {0x80, 6, 0, 1, 0, 0, 18, 0};

    TEST_ASSERT_EQUAL_HEX8_ARRAY(want_setup, x->buf, 8);
    TEST_ASSERT_TRUE(x->in);
    TEST_ASSERT_EQUAL_UINT8(0, x->type);
    TEST_ASSERT_GREATER_OR_EQUAL(8 + 18, x->len);
    complete(1, USBIP_XS_COMPLETED, 8 + 18);
    assert_ret_submit(1, 0, 18);
    TEST_ASSERT_EQUAL_size_t(18, reply(USBIP_RET_SUBMIT, 1)->n);
    TEST_ASSERT_EQUAL_HEX8(17, reply(USBIP_RET_SUBMIT, 1)->data[17]);

    for (int i = 0; i < USBIP_CTRL_DATA_MAX; i++) {
        out_buf[i] = (uint8_t)i;
    }
    control(2, 0x21, 9, 0, 0, USBIP_CTRL_DATA_MAX);
    x = pending(2);
    TEST_ASSERT_NOT_NULL(x);
    TEST_ASSERT_FALSE(x->in);
    TEST_ASSERT_EQUAL_size_t(8 + USBIP_CTRL_DATA_MAX, x->len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(out_buf, x->buf + 8, USBIP_CTRL_DATA_MAX);
    complete(2, USBIP_XS_STALL, 8);
    assert_ret_submit(2, -USBIP_EPIPE, 0);
    TEST_ASSERT_EQUAL_INT(0, log_count("clear"));  // never on EP0
    finish();
}

TEST_CASE("control transfers: one in flight on EP0, the rest wait in order", "[usbip]")
{
    start(cfg_cp, sizeof(cfg_cp));
    control(1, 0x80, 6, 0x0100, 0, 18);
    control(2, 0x80, 6, 0x0200, 0, 9);
    control(3, 0x80, 6, 0x0300, 0, 4);
    TEST_ASSERT_EQUAL_INT(1, log_count("submit"));
    complete(1, USBIP_XS_COMPLETED, 8 + 18);
    complete(2, USBIP_XS_COMPLETED, 8 + 9);
    ASSERT_ORDER("submit 1", "ret_submit 1", "submit 2", "ret_submit 2", "submit 3");
    finish();
}

// --- CMD_UNLINK ---

TEST_CASE("UNLINK, seqnum not pending: answers RET_UNLINK status 0", "[usbip]")
{
    start(cfg_cp, sizeof(cfg_cp));
    submit(1, 0x81, 64);
    complete(1, USBIP_XS_COMPLETED, 2);
    unlink(2, 1);
    assert_ret_unlink(2, 0);
    usbip_counters_t c;

    usbip_engine_counters(&eng, &c);
    TEST_ASSERT_EQUAL_UINT32(1, c.unlinks);
    finish();
}

TEST_CASE("UNLINK, still waits in the endpoint's FIFO: removes it and answers RET_UNLINK -ECONNRESET, no RET_SUBMIT",
          "[usbip]")
{
    start(cfg_cp, sizeof(cfg_cp));
    submit(1, 0x01, 64);
    submit(2, 0x01, 64);
    submit(3, 0x01, 64);
    unlink(4, 2);
    assert_ret_unlink(4, -USBIP_ECONNRESET);
    TEST_ASSERT_EQUAL_INT(-1, log_find("halt 01"));
    complete(1, USBIP_XS_COMPLETED, 64);
    TEST_ASSERT_EQUAL_INT(0, log_count("submit 2"));
    TEST_ASSERT_NOT_NULL(pending(3));
    TEST_ASSERT_NULL(reply(USBIP_RET_SUBMIT, 2));
    finish();
}

TEST_CASE("Unlink of a waiting transfer: exactly one answer for its seqnum, none after the slot frees", "[usbip]")
{
    start(cfg_cp, sizeof(cfg_cp));
    submit(1, 0x81, USBIP_XFER_MAX);
    submit(2, 0x81, USBIP_XFER_MAX);
    submit(3, 0x81, SHARED_REST);  // shared 256 KB full
    submit(4, 0x81, 64);  // waits in PSRAM
    submit(5, 0x81, 64);
    unlink(10, 4);
    complete(1, USBIP_XS_COMPLETED, 0);
    complete(2, USBIP_XS_COMPLETED, 0);
    complete(3, USBIP_XS_COMPLETED, 0);
    complete(5, USBIP_XS_COMPLETED, 0);
    TEST_ASSERT_EQUAL_INT(0, log_count("submit 4"));
    TEST_ASSERT_EQUAL_INT(0, replies_for(USBIP_RET_SUBMIT, 4));
    TEST_ASSERT_EQUAL_INT(1, replies_for(USBIP_RET_UNLINK, 10));
    assert_ret_unlink(10, -USBIP_ECONNRESET);
    usbip_mem_stats_t m;

    usbip_engine_mem(&eng, &m);
    TEST_ASSERT_EQUAL_size_t(0, m.waiting);
    finish();
}

TEST_CASE("UNLINK, in flight on a bulk or interrupt endpoint: answers RET_UNLINK -ECONNRESET and no RET_SUBMIT for it",
          "[usbip]")
{
    start(cfg_cp, sizeof(cfg_cp));
    submit(1, 0x01, 64);
    unlink(2, 1);
    ASSERT_ORDER("halt 01", "flush 01", "clear 01");
    complete(1, USBIP_XS_CANCELED, 0);
    assert_ret_unlink(2, -USBIP_ECONNRESET);
    TEST_ASSERT_NULL(reply(USBIP_RET_SUBMIT, 1));
    TEST_ASSERT_EQUAL_INT(1, log_count("submit"));
    usbip_mem_stats_t m;

    usbip_engine_mem(&eng, &m);
    TEST_ASSERT_EQUAL_size_t(0, m.inflight);
    finish();
}

TEST_CASE("UNLINK, in flight on a bulk or interrupt endpoint: resubmits the endpoint's other canceled transfers in their "
          "original order, ahead of its FIFO", "[usbip]")
{
    start(cfg_cp, sizeof(cfg_cp));
    const int32_t chunk = USBIP_INFLIGHT_BYTES / 8;  // 8 x 32 KB: the shared bytes (not the count) are full

    for (uint32_t s = 1; s <= 8; s++) {
        submit(s, 0x81, chunk);
    }
    submit(9, 0x81, chunk);  // waits
    TEST_ASSERT_NULL(pending(9));
    unlink(20, 3);
    submit(10, 0x81, 64);  // arrives during the flush: behind the FIFO
    // The flush completes everything in flight on 0x81, in any order.
    const uint32_t back[] = {8, 1, 3, 2, 7, 4, 6, 5};

    for (size_t i = 0; i < 8; i++) {
        complete(back[i], USBIP_XS_CANCELED, 0);
    }
    assert_ret_unlink(20, -USBIP_ECONNRESET);
    ASSERT_ORDER("halt 81", "flush 81", "clear 81", "ret_unlink 20", "submit 1", "submit 2", "submit 4", "submit 5",
                 "submit 6", "submit 7", "submit 8", "submit 9");
    TEST_ASSERT_NULL(pending(10));  // the shared budget is full again
    // Resubmitted: each once more; seqnum 3 not again.
    TEST_ASSERT_EQUAL_INT(2, log_count("submit 1"));
    TEST_ASSERT_EQUAL_INT(1, log_count("submit 3"));
    TEST_ASSERT_EQUAL_INT(0, replies_for(USBIP_RET_SUBMIT, 3));
    TEST_ASSERT_EQUAL_INT(1, f.n_rep);
    finish();
}

TEST_CASE("UNLINK, pending on EP0 and completes within 2 s: answers its RET_SUBMIT, then RET_UNLINK status 0",
          "[usbip]")
{
    start(cfg_cp, sizeof(cfg_cp));
    control(1, 0x80, 6, 0x0100, 0, 18);
    f.now = 1000;
    unlink(2, 1);
    TEST_ASSERT_EQUAL_INT(0, f.n_rep);
    TEST_ASSERT_EQUAL_INT(0, log_count("halt"));
    f.now = 2999;
    usbip_engine_tick(&eng);
    TEST_ASSERT_EQUAL_INT(0, f.ends);
    complete(1, USBIP_XS_COMPLETED, 8 + 18);
    ASSERT_ORDER("ret_submit 1", "ret_unlink 2");
    assert_ret_submit(1, 0, 18);
    assert_ret_unlink(2, 0);
    f.now = 10000;
    usbip_engine_tick(&eng);
    TEST_ASSERT_EQUAL_INT(0, f.ends);
    finish();
}

TEST_CASE("UNLINK, pending on EP0 and does not complete within 2 s: ends the session", "[usbip]")
{
    start(cfg_cp, sizeof(cfg_cp));
    control(1, 0x80, 6, 0x0100, 0, 18);
    f.now = 1000;
    unlink(2, 1);
    f.now = 1000 + USBIP_EP0_UNLINK_MS - 1;
    usbip_engine_tick(&eng);
    TEST_ASSERT_EQUAL_INT(0, f.ends);
    f.now = 1000 + USBIP_EP0_UNLINK_MS;
    usbip_engine_tick(&eng);
    TEST_ASSERT_EQUAL_INT(1, f.ends);
    TEST_ASSERT_FALSE(usbip_engine_active(&eng));
    ASSERT_ORDER("end", "power", "release 0");
    TEST_ASSERT_EQUAL_INT(1, log_count("power"));
    finish();
}

TEST_CASE("Completion arrives after the flush began: each seqnum gets exactly one answer", "[usbip]")
{
    start(cfg_cp, sizeof(cfg_cp));
    submit(1, 0x81, 64);
    submit(2, 0x81, 64);
    submit(3, 0x81, 64);
    unlink(10, 2);
    // 1 and 2 had completed in hardware before the flush; their completions were queued behind it.
    complete(1, USBIP_XS_COMPLETED, 7);
    complete(2, USBIP_XS_COMPLETED, 5);
    complete(3, USBIP_XS_CANCELED, 0);
    TEST_ASSERT_EQUAL_INT(1, replies_for(USBIP_RET_SUBMIT, 1));
    TEST_ASSERT_EQUAL_INT(1, replies_for(USBIP_RET_SUBMIT, 2));
    TEST_ASSERT_EQUAL_INT(1, replies_for(USBIP_RET_UNLINK, 10));
    assert_ret_submit(1, 0, 7);
    assert_ret_submit(2, 0, 5);
    assert_ret_unlink(10, 0);  // never -ECONNRESET next to a RET_SUBMIT
    ASSERT_ORDER("ret_submit 2", "ret_unlink 10");
    TEST_ASSERT_EQUAL_INT(1, log_count("submit 1"));  // answered, not resubmitted
    TEST_ASSERT_EQUAL_INT(2, log_count("submit 3"));  // canceled collateral: resubmitted
    finish();
}

// --- Budget pressure ---

TEST_CASE("budget pressure: a waiting transfer goes out when its endpoint frees, before other endpoints' waiting "
          "transfers", "[usbip]")
{
    start(cfg_bt, sizeof(cfg_bt));
    const int32_t part = 80 * 1024;

    submit(1, 0x82, part);
    submit(2, 0x82, USBIP_XFER_MAX);
    submit(3, 0x81, 16);    // reserve: 200 KB + 16 in flight
    submit(4, 0x81, part);  // 280 KB + 16 would exceed 256 KB: waits
    submit(5, 0x82, part);  // waits
    TEST_ASSERT_NULL(pending(4));
    TEST_ASSERT_NULL(pending(5));
    complete(1, USBIP_XS_COMPLETED, 0);
    TEST_ASSERT_NOT_NULL(pending(5));
    TEST_ASSERT_NULL(pending(4));  // only one fits
    finish();
}

TEST_CASE("budget pressure: waiting transfers of one endpoint go out in arrival order", "[usbip]")
{
    start(cfg_cp, sizeof(cfg_cp));
    submit(1, 0x81, USBIP_XFER_MAX);
    submit(2, 0x81, USBIP_XFER_MAX);
    submit(3, 0x81, USBIP_XFER_MAX);
    submit(4, 0x81, USBIP_XFER_MAX);
    submit(5, 0x81, USBIP_XFER_MAX);
    complete(1, USBIP_XS_COMPLETED, 0);
    complete(2, USBIP_XS_COMPLETED, 0);
    complete(3, USBIP_XS_COMPLETED, 0);
    ASSERT_ORDER("submit 1", "submit 2", "submit 3", "submit 4", "submit 5");
    finish();
}

// --- Session end ---

TEST_CASE("session end: halts, flushes and clears every non-control endpoint and releases the interfaces", "[usbip]")
{
    start(cfg_bt, sizeof(cfg_bt));
    submit(1, 0x82, 64);
    usbip_engine_stop(&eng, "detach");
    ASSERT_ORDER("halt 81", "flush 81", "clear 81", "halt 82", "flush 82", "clear 82", "halt 02", "flush 02",
                 "clear 02", "release 0", "release 1");
    TEST_ASSERT_EQUAL_INT(0, log_count("halt 83"));  // iso endpoints are never claimed by the engine
    TEST_ASSERT_EQUAL_INT(0, f.ends);                // the caller ended it
    TEST_ASSERT_EQUAL_INT(0, f.n_rep);
    finish();
}

TEST_CASE("session end: turns the root port off first when a control transfer is still pending", "[usbip]")
{
    start(cfg_cp, sizeof(cfg_cp));
    submit(1, 0x81, 64);
    control(2, 0x80, 6, 0x0100, 0, 18);
    usbip_engine_stop(&eng, "keepalive");
    ASSERT_ORDER("halt 81", "flush 81", "clear 81", "power", "release 0");
    TEST_ASSERT_EQUAL_INT(1, log_count("power"));
    finish();
}

TEST_CASE("session end: power-cycles the root port so the device re-enumerates", "[usbip]")
{
    start(cfg_cp, sizeof(cfg_cp));
    submit(1, 0x81, 64);
    usbip_engine_stop(&eng, "detach");
    ASSERT_ORDER("release 0", "power");
    TEST_ASSERT_EQUAL_INT(1, log_count("power"));
    f.n_log = 0;
    usbip_engine_stop(&eng, "again");  // idempotent
    TEST_ASSERT_EQUAL_INT(0, f.n_log);
    finish();
}

TEST_CASE("Device gone with transfers in flight: all buffers freed", "[usbip]")
{
    start(cfg_cp, sizeof(cfg_cp));
    submit(1, 0x81, USBIP_XFER_MAX);
    submit(2, 0x81, USBIP_XFER_MAX);
    submit(3, 0x81, SHARED_REST);
    submit(4, 0x81, 64);    // waits
    submit(5, 0x01, 64);
    submit(6, 0x01, 128);   // waits, with OUT data in PSRAM
    control(7, 0x80, 6, 0x0100, 0, 18);
    control(8, 0x00, 3, 1, 0, 0);  // waits behind 7
    complete(1, USBIP_XS_NO_DEVICE, 0);
    assert_ret_submit(1, -USBIP_ENODEV, 0);
    TEST_ASSERT_EQUAL_INT(1, f.ends);
    TEST_ASSERT_FALSE(usbip_engine_active(&eng));
    usbip_mem_stats_t m;

    usbip_engine_mem(&eng, &m);
    TEST_ASSERT_EQUAL_size_t(0, m.inflight);
    TEST_ASSERT_EQUAL_size_t(0, m.waiting);
    // The canceled transfers come back after the session ended: freed, nothing sent.
    int reps = f.n_rep;

    complete(2, USBIP_XS_NO_DEVICE, 0);
    complete(3, USBIP_XS_NO_DEVICE, 0);
    complete(5, USBIP_XS_CANCELED, 0);
    complete(7, USBIP_XS_NO_DEVICE, 0);
    TEST_ASSERT_EQUAL_INT(reps, f.n_rep);
    TEST_ASSERT_EQUAL_INT(0, f.n_pend);
    TEST_ASSERT_EQUAL_INT(f.allocs, f.frees);
    finish();
}

TEST_CASE("session end: a transfer of the old session completing after a new start is freed and answers nothing",
          "[usbip]")
{
    start(cfg_cp, sizeof(cfg_cp));
    submit(1, 0x81, 64);
    usbip_xfer_t *old = pending(1);

    usbip_engine_stop(&eng, "detach");
    f.n_pend = 0;  // keep it out of the fake's list across the restart
    int allocs = f.allocs, frees = f.frees;
    usbip_dev_rec_t dev = {.busnum = 1, .devnum = 3, .config_value = 1, .num_ifaces = 1};

    TEST_ASSERT_EQUAL(ESP_OK, usbip_engine_start(&eng, &ops, NULL, &dev, cfg_cp, sizeof(cfg_cp)));
    submit(1, 0x81, 64);  // the new session reuses the seqnum
    usbip_engine_on_complete(&eng, old, USBIP_XS_CANCELED, 0);
    TEST_ASSERT_EQUAL_INT(0, f.n_rep);
    TEST_ASSERT_EQUAL_INT(frees + 2, f.frees);  // the old transfer and its buffer
    TEST_ASSERT_EQUAL_INT(allocs + 2, f.allocs);
    usbip_mem_stats_t m;

    usbip_engine_mem(&eng, &m);
    TEST_ASSERT_EQUAL_size_t(64, m.inflight);  // the new session's transfer still counts
    complete(1, USBIP_XS_COMPLETED, 4);
    assert_ret_submit(1, 0, 4);
    finish();
}

TEST_CASE("session start: claims every interface at alt 0", "[usbip]")
{
    finish_quiet();
    memset(&f, 0, sizeof(f));
    usbip_dev_rec_t dev = {.busnum = 1, .devnum = 3, .config_value = 1, .num_ifaces = 2};

    TEST_ASSERT_EQUAL(ESP_OK, usbip_engine_start(&eng, &ops, NULL, &dev, cfg_bt, sizeof(cfg_bt)));
    ASSERT_ORDER("claim 0 0", "claim 1 0");
    TEST_ASSERT_EQUAL_INT(2, f.n_log);
    TEST_ASSERT_TRUE(usbip_engine_active(&eng));
    finish();
}

TEST_CASE("session start: on a claim failure releases what it claimed and returns the error", "[usbip]")
{
    finish_quiet();
    memset(&f, 0, sizeof(f));
    usbip_dev_rec_t dev = {.busnum = 1, .devnum = 3, .config_value = 1, .num_ifaces = 2};

    f.fail_iface = 1 + 1;
    TEST_ASSERT_EQUAL(ESP_FAIL, usbip_engine_start(&eng, &ops, NULL, &dev, cfg_bt, sizeof(cfg_bt)));
    TEST_ASSERT_FALSE(usbip_engine_active(&eng));
    ASSERT_ORDER("claim 0 0", "claim 1 0", "release 0");
    TEST_ASSERT_EQUAL_INT(1, log_count("release"));
    f.fail_iface = 0;
}

// --- Fix round 1 ---

TEST_CASE("counters and memory peaks: kept across sessions (reset at boot only)", "[usbip]")
{
    start(cfg_cp, sizeof(cfg_cp));
    submit(1, 0x01, OVERLOAD_CHUNK);
    for (uint32_t s = 2; s < 200 && f.ends == 0; s++) {
        submit(s, 0x01, OVERLOAD_CHUNK);
    }
    TEST_ASSERT_EQUAL_INT(1, f.ends);
    finish_quiet();
    usbip_mem_stats_t before;

    usbip_engine_mem(&eng, &before);
    usbip_dev_rec_t dev = {.busnum = 1, .devnum = 3, .config_value = 1, .num_ifaces = 1};

    TEST_ASSERT_EQUAL(ESP_OK, usbip_engine_start(&eng, &ops, NULL, &dev, cfg_cp, sizeof(cfg_cp)));
    usbip_counters_t c;
    usbip_mem_stats_t m;

    usbip_engine_counters(&eng, &c);
    usbip_engine_mem(&eng, &m);
    TEST_ASSERT_EQUAL_UINT32(1, c.overload_ends);
    TEST_ASSERT_GREATER_THAN(60, c.submits);
    TEST_ASSERT_EQUAL_size_t(before.waiting_peak, m.waiting_peak);
    TEST_ASSERT_EQUAL_size_t(before.inflight_peak, m.inflight_peak);
    TEST_ASSERT_GREATER_THAN(USBIP_WAIT_BYTES - OVERLOAD_CHUNK - 1024, m.waiting_peak);
    TEST_ASSERT_EQUAL_size_t(0, m.waiting);
    TEST_ASSERT_EQUAL_size_t(0, m.inflight);
    finish();
}

TEST_CASE("budget pressure: the freed endpoint's FIFO gets its head out, then the others in turn", "[usbip]")
{
    start(cfg_bt, sizeof(cfg_bt));
    const int32_t backlog = 60 * 1024;

    submit(1, 0x82, USBIP_XFER_MAX);
    submit(2, 0x82, USBIP_XFER_MAX);  // 240 KB
    submit(3, 0x81, 16);              // reserve: 240 KB + 16
    submit(4, 0x82, backlog);         // backlog of 0x82
    submit(5, 0x82, backlog);
    submit(6, 0x81, backlog);         // backlog of 0x81
    submit(7, 0x81, backlog);
    complete(1, USBIP_XS_COMPLETED, 0);  // 120 KB + 16 in flight: room for two of 60 KB, not three
    TEST_ASSERT_NOT_NULL(pending(4));
    TEST_ASSERT_NOT_NULL(pending(6));
    TEST_ASSERT_NULL(pending(5));
    TEST_ASSERT_NULL(pending(7));
    finish();
}

TEST_CASE("IN completion with more bytes than asked: answers -EOVERFLOW with actual_length = length", "[usbip]")
{
    start(cfg_cp, sizeof(cfg_cp));
    submit(1, 0x81, 10);  // a 64-byte buffer
    complete(1, USBIP_XS_COMPLETED, 64);
    assert_ret_submit(1, -USBIP_EOVERFLOW, 10);
    TEST_ASSERT_EQUAL_size_t(10, reply(USBIP_RET_SUBMIT, 1)->n);
    control(2, 0x80, 6, 0x0100, 0, 18);  // EP0: 8 + 64-byte buffer
    complete(2, USBIP_XS_COMPLETED, 8 + 20);
    assert_ret_submit(2, -USBIP_EOVERFLOW, 18);
    submit(3, 0x81, 10);
    complete(3, USBIP_XS_COMPLETED, 10);  // exactly what was asked
    assert_ret_submit(3, 0, 10);
    finish();
}

TEST_CASE("UNLINK of a transfer back from a flush but not yet resubmitted: answers -ECONNRESET and never submits it "
          "again", "[usbip]")
{
    start(cfg_cp, sizeof(cfg_cp));
    submit(1, 0x81, 64);
    submit(2, 0x81, 64);
    submit(3, 0x81, 64);
    unlink(10, 2);
    complete(1, USBIP_XS_CANCELED, 0);  // back, waits for 2 and 3
    unlink(11, 1);
    assert_ret_unlink(11, -USBIP_ECONNRESET);
    complete(2, USBIP_XS_CANCELED, 0);
    complete(3, USBIP_XS_CANCELED, 0);
    assert_ret_unlink(10, -USBIP_ECONNRESET);
    TEST_ASSERT_EQUAL_INT(1, log_count("submit 1"));
    TEST_ASSERT_EQUAL_INT(2, log_count("submit 3"));
    TEST_ASSERT_EQUAL_INT(0, replies_for(USBIP_RET_SUBMIT, 1));
    TEST_ASSERT_EQUAL_INT(2, f.n_rep);
    finish();
}

TEST_CASE("UNLINK of a seqnum already being unlinked: the second answers 0, the first -ECONNRESET", "[usbip]")
{
    start(cfg_cp, sizeof(cfg_cp));
    submit(1, 0x81, 64);
    unlink(10, 1);
    unlink(11, 1);
    assert_ret_unlink(11, 0);
    TEST_ASSERT_EQUAL_INT(1, log_count("flush 81"));
    complete(1, USBIP_XS_CANCELED, 0);
    assert_ret_unlink(10, -USBIP_ECONNRESET);
    TEST_ASSERT_EQUAL_INT(0, replies_for(USBIP_RET_SUBMIT, 1));
    TEST_ASSERT_EQUAL_INT(2, f.n_rep);
    finish();
}

TEST_CASE("SET_INTERFACE: when the USB side refuses the forward, claims the previous alt and answers -EPROTO",
          "[usbip]")
{
    start(cfg_alt, sizeof(cfg_alt));
    f.submit_ret = ESP_ERR_INVALID_STATE;
    control(10, 0x01, 11, 1, 0, 0);
    f.submit_ret = ESP_OK;
    ASSERT_ORDER("release 0", "submit 10", "claim 0 0", "ret_submit 10");
    assert_ret_submit(10, -USBIP_EPROTO, 0);
    submit(11, 0x83, 8);
    assert_ret_submit(11, -USBIP_EINVAL, 0);
    submit(12, 0x81, 64);
    TEST_ASSERT_NOT_NULL(pending(12));
    finish();
}

TEST_CASE("DMA buffer allocation fails: answers -ENOMEM and frees the transfer", "[usbip]")
{
    start(cfg_cp, sizeof(cfg_cp));
    f.fail_dma = true;
    submit(1, 0x81, 64);
    f.fail_dma = false;
    assert_ret_submit(1, -USBIP_ENOMEM, 0);
    TEST_ASSERT_EQUAL_INT(0, log_count("submit"));
    usbip_mem_stats_t m;

    usbip_engine_mem(&eng, &m);
    TEST_ASSERT_EQUAL_size_t(0, m.inflight);
    TEST_ASSERT_EQUAL_INT(f.allocs, f.frees);
    submit(2, 0x81, 64);  // the endpoint's reserve is free again
    TEST_ASSERT_NOT_NULL(pending(2));
    finish();
}
