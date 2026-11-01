#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "unity.h"
#include "unity_test_runner.h"
#include "br_core.h"
#include "sniffer.h"
#include "golden_frame.h"

#define GOLDEN_TS_US 1234567u
#define GOLDEN_CHANNEL 15
#define GOLDEN_RSSI (-60)
#define GOLDEN_LQI 200

/* ---- fake br_core ---- */

static struct {
    esp_err_t enter_err;
    esp_err_t leave_err;
    int enter_calls;
    int leave_calls;
    uint8_t channel;
    otLinkRawReceiveDone rx_cb;
    void (*end_cb)(void);
    br_core_mode_t mode;
} fake;

esp_err_t br_core_enter_sniffer(uint8_t channel, otLinkRawReceiveDone receive_cb)
{
    fake.enter_calls++;
    fake.channel = channel;
    fake.rx_cb = receive_cb;
    return fake.enter_err;
}

esp_err_t br_core_leave_sniffer(void)
{
    fake.leave_calls++;
    return fake.leave_err;
}

br_core_mode_t br_core_get_mode(void)
{
    return fake.mode;
}

void br_core_set_sniffer_end_cb(void (*cb)(void))
{
    fake.end_cb = cb;
}

/* ---- helpers ---- */

static void setup(void)
{
    sniffer_stop(); /* whatever an earlier test left behind */
    void (*end_cb)(void) = fake.end_cb;
    memset(&fake, 0, sizeof(fake));
    fake.end_cb = end_cb;
    fake.mode = BR_MODE_SNIFFER;
}

static void inject(uint64_t ts_us, uint8_t channel, int8_t rssi, uint8_t lqi, const uint8_t *psdu, uint16_t len)
{
    uint8_t copy[SNIFFER_MAX_PSDU_LEN + 1];
    memcpy(copy, psdu, len);
    otRadioFrame frame = {.mPsdu = copy, .mLength = len, .mChannel = channel};
    frame.mInfo.mRxInfo.mTimestamp = ts_us;
    frame.mInfo.mRxInfo.mRssi = rssi;
    frame.mInfo.mRxInfo.mLqi = lqi;
    fake.rx_cb(NULL, &frame, OT_ERROR_NONE);
}

static void inject_golden(void)
{
    inject(GOLDEN_TS_US, GOLDEN_CHANNEL, GOLDEN_RSSI, GOLDEN_LQI, golden_psdu, sizeof(golden_psdu));
}

static sniffer_stats_t stats(void)
{
    sniffer_stats_t st;
    sniffer_get_stats(&st);
    return st;
}

static sniffer_frame_t golden_frame(void);
/* The record the capture writes for the golden frame (the RCP's trailing two bytes are not an FCS). */
static uint8_t s_captured[256];
static size_t s_captured_len;
static const uint8_t *captured_record(void);
static size_t captured_len(void);

static sniffer_frame_t golden_frame(void)
{
    sniffer_frame_t f = {.timestamp_us = GOLDEN_TS_US, .channel = GOLDEN_CHANNEL, .rssi = GOLDEN_RSSI,
                         .lqi = GOLDEN_LQI, .psdu_len = sizeof(golden_psdu)
                        };
    memcpy(f.psdu, golden_psdu, sizeof(golden_psdu));
    return f;
}

static size_t captured_len(void)
{
    if (s_captured_len == 0) {
        const sniffer_frame_t f = golden_frame();
        s_captured_len = sniffer_pcap_record(&f, SNIFFER_RX_PSDU_HAS_FCS, s_captured, sizeof(s_captured));
    }
    return s_captured_len;
}

static const uint8_t *captured_record(void)
{
    captured_len();
    return s_captured;
}

static uint32_t u32_at(const uint8_t *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ---- pcap encoding ---- */

TEST_CASE("On global header: is 24 bytes with magic a1b2c3d4 and linktype 283", "[sniffer]")
{
    uint8_t buf[64];
    TEST_ASSERT_EQUAL(24, sniffer_pcap_global_header(buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_HEX32(0xa1b2c3d4, u32_at(buf));
    TEST_ASSERT_EQUAL(283, u32_at(buf + 20));
}

TEST_CASE("On global header: version is 2.4 and snaplen 256, byte-exact", "[sniffer]")
{
    uint8_t buf[24];
    sniffer_pcap_global_header(buf, sizeof(buf));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(golden_global_header, buf, sizeof(buf));
}

TEST_CASE("On global header into a buffer under 24 bytes: writes nothing and returns 0", "[sniffer]")
{
    uint8_t buf[23] = {0};
    TEST_ASSERT_EQUAL(0, sniffer_pcap_global_header(buf, sizeof(buf)));
}

TEST_CASE("On frame with valid FCS: FCS TLV value 1 and PSDU includes FCS", "[sniffer]")
{
    const sniffer_frame_t f = golden_frame();
    uint8_t buf[256];
    const size_t n = sniffer_pcap_record(&f, true, buf, sizeof(buf));
    TEST_ASSERT_EQUAL(16 + 36 + sizeof(golden_psdu), n);
    TEST_ASSERT_EQUAL(0, buf[16 + 4]);         /* TLV type 0 (FCS type), low byte */
    TEST_ASSERT_EQUAL(1, buf[16 + 8]);         /* its value */
    TEST_ASSERT_EQUAL_HEX8_ARRAY(golden_psdu, buf + 16 + 36, sizeof(golden_psdu));
}

TEST_CASE("On frame without valid FCS: FCS TLV value 0 and FCS bytes stripped", "[sniffer]")
{
    const sniffer_frame_t f = golden_frame();
    uint8_t buf[256];
    const size_t n = sniffer_pcap_record(&f, false, buf, sizeof(buf));
    TEST_ASSERT_EQUAL(16 + 36 + sizeof(golden_psdu) - 2, n);
    TEST_ASSERT_EQUAL(0, buf[16 + 8]);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(golden_psdu, buf + 16 + 36, sizeof(golden_psdu) - 2);
}

TEST_CASE("On any record: incl_len equals TAP header length plus PSDU length", "[sniffer]")
{
    sniffer_frame_t f = golden_frame();
    uint8_t buf[256];
    for (int fcs = 0; fcs < 2; fcs++) {
        const size_t n = sniffer_pcap_record(&f, fcs, buf, sizeof(buf));
        const size_t psdu = fcs ? f.psdu_len : f.psdu_len - 2;
        TEST_ASSERT_EQUAL(36 + psdu, u32_at(buf + 8));
        TEST_ASSERT_EQUAL(u32_at(buf + 8), u32_at(buf + 12));
        TEST_ASSERT_EQUAL(n, 16 + u32_at(buf + 8));
        TEST_ASSERT_EQUAL(36, buf[16 + 2]); /* TAP header length field */
    }
}

TEST_CASE("On a known frame: the record is byte-exact the golden vector (checked with tshark)", "[sniffer]")
{
    const sniffer_frame_t f = golden_frame();
    uint8_t buf[256];
    const size_t n = sniffer_pcap_record(&f, true, buf, sizeof(buf));
    TEST_ASSERT_EQUAL(sizeof(golden_record), n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(golden_record, buf, n);
}

TEST_CASE("On a buffer too small for the record: writes nothing and returns 0", "[sniffer]")
{
    const sniffer_frame_t f = golden_frame();
    uint8_t buf[256];
    TEST_ASSERT_EQUAL(0, sniffer_pcap_record(&f, true, buf, sizeof(golden_record) - 1));
}

TEST_CASE("On a 127-byte frame: the record fits the snaplen", "[sniffer]")
{
    sniffer_frame_t f = {.psdu_len = 127};
    uint8_t buf[256];
    const size_t n = sniffer_pcap_record(&f, true, buf, sizeof(buf));
    TEST_ASSERT_EQUAL(16 + 36 + 127, n);
    TEST_ASSERT_TRUE(u32_at(buf + 8) <= 256);
}

/* ---- capture ---- */

TEST_CASE("On frames shorter than 3 bytes: counts drops and a later valid frame is still read", "[sniffer]")
{
    setup();
    TEST_ASSERT_EQUAL(ESP_OK, sniffer_start(15, 0));
    uint8_t buf[512];
    const uint8_t junk[2] = {1, 2};
    sniffer_read(buf, sizeof(buf), 10); /* header */
    for (uint16_t len = 0; len <= 2; len++) {
        inject(len, 15, -50, 10, junk, len);
    }
    inject_golden();
    TEST_ASSERT_EQUAL(3, stats().drops);
    TEST_ASSERT_EQUAL(1, stats().frames);
    TEST_ASSERT_EQUAL(captured_len(), sniffer_read(buf, sizeof(buf), 100));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(captured_record(), buf, captured_len());
}

TEST_CASE("On start: enters the sniffer mode on the channel with a receive callback", "[sniffer]")
{
    setup();
    TEST_ASSERT_EQUAL(ESP_OK, sniffer_start(20, 0));
    TEST_ASSERT_EQUAL(1, fake.enter_calls);
    TEST_ASSERT_EQUAL(20, fake.channel);
    TEST_ASSERT_NOT_NULL(fake.rx_cb);
    TEST_ASSERT_NOT_NULL(fake.end_cb);
    TEST_ASSERT_TRUE(stats().active);
    TEST_ASSERT_EQUAL(20, stats().channel);
}

TEST_CASE("On frame received: appends a pcap record with channel, RSSI, LQI TLVs and timestamp", "[sniffer]")
{
    setup();
    TEST_ASSERT_EQUAL(ESP_OK, sniffer_start(15, 0));
    inject_golden();
    uint8_t buf[512];
    const size_t n = sniffer_read(buf, sizeof(buf), 100);
    TEST_ASSERT_EQUAL(24 + captured_len(), n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(golden_global_header, buf, 24);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(captured_record(), buf + 24, captured_len());
    TEST_ASSERT_EQUAL(1, stats().frames);
}

TEST_CASE("On read: the global header comes first, once", "[sniffer]")
{
    setup();
    TEST_ASSERT_EQUAL(ESP_OK, sniffer_start(15, 0));
    uint8_t buf[512];
    TEST_ASSERT_EQUAL(24, sniffer_read(buf, sizeof(buf), 100));
    TEST_ASSERT_EQUAL_HEX32(0xa1b2c3d4, u32_at(buf));
    inject_golden();
    TEST_ASSERT_EQUAL(captured_len(), sniffer_read(buf, sizeof(buf), 100));
}

TEST_CASE("On read without a frame: returns 0 after the timeout while still active", "[sniffer]")
{
    setup();
    TEST_ASSERT_EQUAL(ESP_OK, sniffer_start(15, 0));
    uint8_t buf[512];
    sniffer_read(buf, sizeof(buf), 100); /* header */
    TEST_ASSERT_EQUAL(0, sniffer_read(buf, sizeof(buf), 50));
    TEST_ASSERT_TRUE(stats().active);
}

TEST_CASE("On read with several frames and a small buffer: returns whole records in order", "[sniffer]")
{
    setup();
    TEST_ASSERT_EQUAL(ESP_OK, sniffer_start(15, 0));
    uint8_t psdu[SNIFFER_MAX_PSDU_LEN] = {0};
    const size_t record = 16 + SNIFFER_TAP_HEADER_LEN + sizeof(psdu) - (SNIFFER_RX_PSDU_HAS_FCS ? 0 : 2);
    inject(1, 15, -50, 10, psdu, sizeof(psdu));
    inject(2, 15, -51, 11, psdu, sizeof(psdu));
    uint8_t buf[SNIFFER_MIN_READ_CAP]; /* header plus one longest record fit, a second one does not */
    TEST_ASSERT_EQUAL(24 + record, sniffer_read(buf, sizeof(buf), 100));
    TEST_ASSERT_EQUAL(1, u32_at(buf + 24 + 4)); /* ts_usec of the first record */
    TEST_ASSERT_EQUAL(record, sniffer_read(buf, sizeof(buf), 100));
    TEST_ASSERT_EQUAL(2, u32_at(buf + 4));
}

TEST_CASE("On ring buffer full: drops the frame", "[sniffer]")
{
    setup();
    TEST_ASSERT_EQUAL(ESP_OK, sniffer_start(15, 0));
    uint8_t psdu[127] = {0};
    const uint32_t capacity = SNIFFER_RING_BYTES / (12 + sizeof(psdu));
    for (uint32_t i = 0; i < capacity + 50; i++) {
        inject(i, 15, -50, 1, psdu, sizeof(psdu));
    }
    TEST_ASSERT_EQUAL(capacity, stats().frames);
}

TEST_CASE("On ring buffer full: increments the drop counter", "[sniffer]")
{
    setup();
    TEST_ASSERT_EQUAL(ESP_OK, sniffer_start(15, 0));
    uint8_t psdu[127] = {0};
    const uint32_t capacity = SNIFFER_RING_BYTES / (12 + sizeof(psdu));
    for (uint32_t i = 0; i < capacity + 50; i++) {
        inject(i, 15, -50, 1, psdu, sizeof(psdu));
    }
    TEST_ASSERT_EQUAL(50, stats().drops);
}

TEST_CASE("On ring buffer full and then read: space is free again and new frames are stored", "[sniffer]")
{
    setup();
    TEST_ASSERT_EQUAL(ESP_OK, sniffer_start(15, 0));
    uint8_t psdu[127] = {0};
    const uint32_t capacity = SNIFFER_RING_BYTES / (12 + sizeof(psdu));
    for (uint32_t i = 0; i < capacity + 1; i++) {
        inject(i, 15, -50, 1, psdu, sizeof(psdu));
    }
    uint8_t buf[1024];
    TEST_ASSERT_TRUE(sniffer_read(buf, sizeof(buf), 10) > 0);
    inject(99, 15, -50, 1, psdu, sizeof(psdu));
    TEST_ASSERT_EQUAL(capacity + 1, stats().frames);
    TEST_ASSERT_EQUAL(1, stats().drops);
}

TEST_CASE("On frame wrapping around the end of the ring: is read back intact", "[sniffer]")
{
    setup();
    TEST_ASSERT_EQUAL(ESP_OK, sniffer_start(15, 0));
    uint8_t buf[1024];
    sniffer_read(buf, sizeof(buf), 10); /* header */
    /* Cycle enough 22-byte-PSDU entries (12 + 10 payload bytes each) through the ring to pass its end several times. */
    for (int round = 0; round < 3 * (SNIFFER_RING_BYTES / 22); round += 100) {
        for (int i = 0; i < 100; i++) {
            inject(round + i, 15, GOLDEN_RSSI, GOLDEN_LQI, golden_psdu, sizeof(golden_psdu));
        }
        size_t got = 0;
        while (got < 100 * captured_len()) {
            const size_t n = sniffer_read(buf, sizeof(buf), 10);
            TEST_ASSERT_TRUE(n > 0);
            TEST_ASSERT_EQUAL(0, n % captured_len());
            for (size_t off = 0; off < n; off += captured_len()) {
                TEST_ASSERT_EQUAL_HEX8_ARRAY(captured_record() + 8, buf + off + 8, captured_len() - 8);
            }
            got += n;
        }
    }
    TEST_ASSERT_EQUAL(0, stats().drops);
}

TEST_CASE("On start while active: returns ESP_ERR_INVALID_STATE", "[sniffer]")
{
    setup();
    TEST_ASSERT_EQUAL(ESP_OK, sniffer_start(15, 0));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, sniffer_start(16, 0));
    TEST_ASSERT_EQUAL(1, fake.enter_calls);
    TEST_ASSERT_EQUAL(15, stats().channel);
}

TEST_CASE("On start when br_core refuses: returns its error and the sniffer stays inactive", "[sniffer]")
{
    setup();
    fake.enter_err = ESP_ERR_INVALID_STATE;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, sniffer_start(15, 0));
    TEST_ASSERT_FALSE(stats().active);
    fake.enter_err = ESP_OK;
    TEST_ASSERT_EQUAL(ESP_OK, sniffer_start(15, 0)); /* a new start works again */
}

TEST_CASE("On start when br_core refuses: no frame is stored afterwards", "[sniffer]")
{
    setup();
    fake.enter_err = ESP_FAIL;
    TEST_ASSERT_EQUAL(ESP_FAIL, sniffer_start(15, 0));
    inject_golden();
    TEST_ASSERT_EQUAL(0, stats().frames);
}

/* ---- stream ---- */

TEST_CASE("On stop: leaves the sniffer mode through br_core and becomes inactive", "[sniffer]")
{
    setup();
    TEST_ASSERT_EQUAL(ESP_OK, sniffer_start(15, 0));
    TEST_ASSERT_EQUAL(ESP_OK, sniffer_stop());
    TEST_ASSERT_EQUAL(1, fake.leave_calls);
    TEST_ASSERT_FALSE(stats().active);
}

TEST_CASE("On stop: frees the ring so that reading returns 0 and a second stop is refused", "[sniffer]")
{
    setup();
    TEST_ASSERT_EQUAL(ESP_OK, sniffer_start(15, 0));
    TEST_ASSERT_EQUAL(ESP_OK, sniffer_stop());
    uint8_t buf[512];
    TEST_ASSERT_EQUAL(0, sniffer_read(buf, sizeof(buf), 10));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, sniffer_stop());
}

TEST_CASE("On stop when br_core fails to leave: returns the error and stays active", "[sniffer]")
{
    setup();
    TEST_ASSERT_EQUAL(ESP_OK, sniffer_start(15, 0));
    fake.leave_err = ESP_FAIL;
    TEST_ASSERT_EQUAL(ESP_FAIL, sniffer_stop());
    TEST_ASSERT_TRUE(stats().active);
    fake.leave_err = ESP_OK;
    TEST_ASSERT_EQUAL(ESP_OK, sniffer_stop());
}

TEST_CASE("On stop when someone else already left the sniffer mode: returns ESP_OK and the sniffer is inactive", "[sniffer]")
{
    setup();
    TEST_ASSERT_EQUAL(ESP_OK, sniffer_start(15, 0));
    fake.leave_err = ESP_ERR_INVALID_STATE;
    fake.mode = BR_MODE_BR_RUNNING;
    TEST_ASSERT_EQUAL(ESP_OK, sniffer_stop());
    TEST_ASSERT_FALSE(stats().active);
    TEST_ASSERT_EQUAL(ESP_OK, sniffer_start(15, 0)); /* not wedged */
}

TEST_CASE("On read when someone else left the sniffer mode at the duration end: the capture ends instead of wedging", "[sniffer]")
{
    setup();
    TEST_ASSERT_EQUAL(ESP_OK, sniffer_start(15, 1));
    fake.leave_err = ESP_ERR_INVALID_STATE;
    fake.mode = BR_MODE_BR_RUNNING;
    uint8_t buf[512];
    TEST_ASSERT_EQUAL(24, sniffer_read(buf, sizeof(buf), 100)); /* header */
    TEST_ASSERT_EQUAL(0, sniffer_read(buf, sizeof(buf), 3000));
    TEST_ASSERT_FALSE(stats().active);
}

TEST_CASE("On stop while a reader waits: the read returns 0", "[sniffer]")
{
    setup();
    TEST_ASSERT_EQUAL(ESP_OK, sniffer_start(15, 0));
    uint8_t buf[512];
    sniffer_read(buf, sizeof(buf), 10); /* header */
    TEST_ASSERT_EQUAL(ESP_OK, sniffer_stop());
    TEST_ASSERT_EQUAL(0, sniffer_read(buf, sizeof(buf), 2000)); /* returns at once, not after 2 s */
}

TEST_CASE("On duration elapsed: leaves the sniffer mode, the captured data is still read, then 0", "[sniffer]")
{
    setup();
    TEST_ASSERT_EQUAL(ESP_OK, sniffer_start(15, 1));
    inject_golden();
    uint8_t buf[512];
    TEST_ASSERT_EQUAL(24 + captured_len(), sniffer_read(buf, sizeof(buf), 100));
    TEST_ASSERT_EQUAL(0, fake.leave_calls);
    TEST_ASSERT_EQUAL(0, sniffer_read(buf, sizeof(buf), 3000)); /* ends after about 1 s */
    TEST_ASSERT_EQUAL(1, fake.leave_calls);
    TEST_ASSERT_FALSE(stats().active);
    TEST_ASSERT_FALSE(stats().ended_by_fault);
    TEST_ASSERT_EQUAL(1, stats().elapsed_s);
    TEST_ASSERT_EQUAL(1, stats().duration_s);
}

TEST_CASE("On frames still unread when the duration elapsed: they are returned before the end", "[sniffer]")
{
    setup();
    TEST_ASSERT_EQUAL(ESP_OK, sniffer_start(15, 1));
    uint8_t buf[512];
    sniffer_read(buf, sizeof(buf), 10); /* header */
    inject_golden();
    vTaskDelay(pdMS_TO_TICKS(2000));
    TEST_ASSERT_EQUAL(captured_len(), sniffer_read(buf, sizeof(buf), 100));
    TEST_ASSERT_EQUAL(1, fake.leave_calls);
    TEST_ASSERT_EQUAL(0, sniffer_read(buf, sizeof(buf), 100));
}

TEST_CASE("On frame after the capture ended: is not stored", "[sniffer]")
{
    setup();
    TEST_ASSERT_EQUAL(ESP_OK, sniffer_start(15, 0));
    otLinkRawReceiveDone cb = fake.rx_cb;
    TEST_ASSERT_EQUAL(ESP_OK, sniffer_stop());
    fake.rx_cb = cb;
    inject_golden();
    TEST_ASSERT_EQUAL(0, stats().frames);
}

TEST_CASE("On br_core ending the capture (RCP failure): the sniffer becomes inactive and flags ended_by_fault", "[sniffer]")
{
    setup();
    TEST_ASSERT_EQUAL(ESP_OK, sniffer_start(15, 0));
    fake.end_cb();
    TEST_ASSERT_FALSE(stats().active);
    TEST_ASSERT_TRUE(stats().ended_by_fault);
    TEST_ASSERT_EQUAL(15, stats().channel);
}

TEST_CASE("On br_core ending the capture: the data captured so far is read, then 0 and the ring is freed", "[sniffer]")
{
    setup();
    TEST_ASSERT_EQUAL(ESP_OK, sniffer_start(15, 0));
    inject_golden();
    fake.end_cb();
    uint8_t buf[512];
    TEST_ASSERT_EQUAL(24 + captured_len(), sniffer_read(buf, sizeof(buf), 100));
    TEST_ASSERT_EQUAL(0, sniffer_read(buf, sizeof(buf), 2000));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, sniffer_stop()); /* no ring left */
    TEST_ASSERT_EQUAL(0, fake.leave_calls);
}

TEST_CASE("On br_core ending the capture while a reader waits: the read returns 0 at once", "[sniffer]")
{
    setup();
    TEST_ASSERT_EQUAL(ESP_OK, sniffer_start(15, 0));
    uint8_t buf[512];
    sniffer_read(buf, sizeof(buf), 10); /* header */
    fake.end_cb();
    TEST_ASSERT_EQUAL(0, sniffer_read(buf, sizeof(buf), 5000));
}

TEST_CASE("On stop after br_core ended the capture: returns ESP_OK without leaving again", "[sniffer]")
{
    setup();
    TEST_ASSERT_EQUAL(ESP_OK, sniffer_start(15, 0));
    fake.end_cb();
    TEST_ASSERT_EQUAL(ESP_OK, sniffer_stop());
    TEST_ASSERT_EQUAL(0, fake.leave_calls);
}

TEST_CASE("On start after the capture ended by a fault: a new capture starts and the flag clears", "[sniffer]")
{
    setup();
    TEST_ASSERT_EQUAL(ESP_OK, sniffer_start(15, 0));
    fake.end_cb();
    TEST_ASSERT_EQUAL(ESP_OK, sniffer_start(17, 0));
    TEST_ASSERT_TRUE(stats().active);
    TEST_ASSERT_FALSE(stats().ended_by_fault);
    TEST_ASSERT_EQUAL(0, stats().frames);
}

void app_main(void)
{
    UNITY_BEGIN();
    unity_run_all_tests();
    int failures = (int)UNITY_END();
    exit(failures == 0 ? 0 : 1);
}
