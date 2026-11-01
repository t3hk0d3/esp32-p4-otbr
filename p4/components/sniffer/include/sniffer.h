#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SNIFFER_PCAP_LINKTYPE_IEEE802_15_4_TAP 283
#define SNIFFER_PCAP_GLOBAL_HEADER_LEN 24
#define SNIFFER_PCAP_SNAPLEN 256
#define SNIFFER_TAP_HEADER_LEN 36 /**< TAP header (4) plus the FCS, RSS, channel and LQI TLVs (8 each) */
#define SNIFFER_MAX_PSDU_LEN 127
#define SNIFFER_RING_BYTES (256 * 1024)
/** Smallest buffer sniffer_read accepts: the global header or one record of the longest frame. */
#define SNIFFER_MIN_READ_CAP 256

typedef struct {
    uint64_t timestamp_us;
    uint8_t channel;
    int8_t rssi;
    uint8_t lqi;
    uint8_t psdu_len; /**< includes the 2 FCS bytes when the radio delivers them (Phase 0: it does) */
    uint8_t psdu[SNIFFER_MAX_PSDU_LEN];
} sniffer_frame_t;

/** pcap global header (24 bytes): magic a1b2c3d4, version 2.4, snaplen 256, linktype 283. 0 if `cap` < 24. */
size_t sniffer_pcap_global_header(uint8_t *buf, size_t cap);

/**
 * pcap record header, IEEE 802.15.4 TAP header (TLVs: FCS type, RSS float32 dBm, channel assignment with page 0,
 * LQI, each padded to 4 bytes) and the PSDU. With `fcs_valid` the FCS type is 1 (16-bit) and the PSDU is written
 * whole; without it the type is 0 and the last 2 PSDU bytes are stripped. Returns the bytes written, 0 if `cap`
 * is too small or the frame is invalid (psdu_len above 127, or below 2 without fcs_valid).
 */
size_t sniffer_pcap_record(const sniffer_frame_t *f, bool fcs_valid, uint8_t *buf, size_t cap);

/**
 * Whether the PSDU the RCP delivers ends in a valid FCS. It does not: the ESP 802.15.4 driver puts RSSI and LQI into
 * the two trailing bytes (the radio checked the real FCS already). Found on the board with Wireshark, where the
 * trailing bytes equalled the RSSI and LQI TLVs. The capture drops them and the TAP header says "no FCS".
 */
#define SNIFFER_RX_PSDU_HAS_FCS false

/**
 * Start a capture on `channel` (11..26) for `duration_s` seconds (0 = until sniffer_stop): allocate the 256 KB ring
 * in PSRAM and enter the sniffer mode through br_core. ESP_ERR_INVALID_STATE while a capture is active (an ended
 * capture whose data is still unread is discarded), ESP_ERR_NO_MEM without PSRAM for the ring, otherwise the
 * br_core_enter_sniffer error (the ring is freed again).
 */
esp_err_t sniffer_start(uint8_t channel, uint32_t duration_s);

/**
 * Next pcap bytes (the global header comes first), at most `cap` (>= SNIFFER_MIN_READ_CAP), waiting up to
 * `timeout_ms` for a frame. Returns 0 on timeout and when the capture has ended and everything was read; tell them
 * apart with sniffer_get_stats().active. The duration is enforced here (the capture leaves the sniffer mode when it
 * has run for `duration_s`, the data already captured is still returned). Frees the ring once an ended capture is
 * drained. One reader at a time.
 */
size_t sniffer_read(uint8_t *buf, size_t cap, uint32_t timeout_ms);

/**
 * Leave the sniffer mode through br_core and free the ring. ESP_OK as well when the capture had already ended on
 * its own (its leftover data is discarded); ESP_ERR_INVALID_STATE when there is no capture.
 */
esp_err_t sniffer_stop(void);

typedef struct {
    bool active;           /**< capturing; false once stopped, ended by the duration or ended by a fault */
    bool ended_by_fault;   /**< the capture ended because br_core left the sniffer mode (RCP failure, radio fault) */
    uint8_t channel;       /**< channel of the current or last capture */
    uint32_t frames;       /**< frames stored in the ring */
    uint32_t drops;        /**< frames dropped because the ring was full */
    uint32_t elapsed_s;
    uint32_t duration_s;   /**< 0 = unlimited */
} sniffer_stats_t;

void sniffer_get_stats(sniffer_stats_t *out);

#ifdef __cplusplus
}
#endif
