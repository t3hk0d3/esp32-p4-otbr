// Firmware detection of a USB radio stick: the probe sequencer and the answer parsers. Pure: no ESP-IDF calls, no I/O,
// no heap. The board code drives it: it opens the device at *baud, writes tx, collects bytes until wait_until_ms or an
// answer, and calls again with what it collected.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "radio_mux_core.h"
#include "radio_mux_hdlc.h"

#ifdef __cplusplus
extern "C" {
#endif

// The whole detection takes at most this long from radio_mux_detect_start.
#define RADIO_MUX_DETECT_CAP_MS 3000
// Per-step budgets: spinel 460800, spinel 115200, ZNP 115200, EZSP ASH 115200, EZSP ASH 460800.
#define RADIO_MUX_DETECT_SPINEL_MS 350
#define RADIO_MUX_DETECT_ZNP_MS 400
#define RADIO_MUX_DETECT_EZSP_MS 800
// After a spinel answer: the wait for the answer to the Ziggurat property request.
#define RADIO_MUX_DETECT_VENDOR_MS 200

// The Ziggurat spinel firmware answers a PROP_VALUE_GET of this property with PROP_VALUE_IS; other spinel firmware
// refuses it (LAST_STATUS) or stays silent. SPINEL_PROP_VENDOR__BEGIN (0x3C00) + 346, from the upstream Ziggurat
// firmware and zigpy-ziggurat. Packed uint: DA 7A. Detection only ever sends a GET on it, never a PROP_VALUE_SET: a SET
// starts the embedded Zigbee stack.
#define SPINEL_PROP_VENDOR_ZIGGURAT 0x3D5A

typedef struct {
    radio_mux_fw_t type;
    char version[48];
    char chip[24];
    uint32_t baud; // the baud the winning answer came at; 0 when unknown
} radio_mux_fw_record_t;

typedef enum {
    RADIO_MUX_PROBE_SEND, // reopen at *baud, write tx (tx_n may be 0: keep collecting), collect until *wait_until_ms
    RADIO_MUX_PROBE_DONE, // `result` is filled; the detection is over
} radio_mux_probe_action_t;

#define RADIO_MUX_DETECT_RX_MAX 160 // longest spinel frame (without CRC) the detector takes: 158
#define RADIO_MUX_DETECT_TX_MAX 16
#define RADIO_MUX_DETECT_ACC_MAX 32

typedef struct {
    uint8_t stage, step;
    int64_t start_ms, deadline_ms;
    radio_mux_fw_record_t rec;
    radio_mux_hdlc_dec_t dec;
    uint8_t dec_buf[RADIO_MUX_DETECT_RX_MAX];
    uint8_t tx[RADIO_MUX_DETECT_TX_MAX];
    size_t tx_n;
    uint8_t acc[RADIO_MUX_DETECT_ACC_MAX]; // ZNP / ASH bytes of the frame in progress
    size_t acc_n;
    bool ash_esc;
} radio_mux_detect_t;

void radio_mux_detect_start(radio_mux_detect_t *d, int64_t now_ms);

// One step of the sequence. The first call after _start (rx_n 0) asks for the first probe. Every call fills *baud and
// *wait_until_ms. SEND with *tx_n > 0: write those bytes (valid until the next call); SEND with *tx_n 0: nothing new,
// keep collecting until *wait_until_ms. `rx` are the bytes collected since the previous call. A call at or after
// start + RADIO_MUX_DETECT_CAP_MS finishes with unknown whatever the step (a spinel firmware seen by then is kept).
radio_mux_probe_action_t radio_mux_detect_next(radio_mux_detect_t *d, int64_t now_ms, const uint8_t *rx, size_t rx_n,
                                               uint32_t *baud, const uint8_t **tx, size_t *tx_n,
                                               int64_t *wait_until_ms, radio_mux_fw_record_t *result);

// "OPENTHREAD/1.4.0-Koenkk-2025.3.1; CC13XX_CC26XX; Mar 13 2025" -> version "1.4.0-Koenkk-2025.3.1", chip
// "CC13XX_CC26XX": the text after the first '/' of the first field (the whole field when there is no '/') and the
// second field, both trimmed and cut to fit. False only for NULL or an empty string.
bool radio_mux_detect_parse_spinel_version(const char *s, char *version, size_t vcap, char *chip, size_t ccap);

#ifdef __cplusplus
}
#endif
