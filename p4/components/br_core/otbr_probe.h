// The RCP probe before OpenThread starts on a channel (spec section 3, OTBR mode): spinel reset, PROP_NCP_VERSION, the
// capabilities. The frames and the checks of the answers; br_core.c sends and waits. Pure: no ESP-IDF calls.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Spinel values the probe uses (lib/spinel/spinel.h; this file stays free of OpenThread).
#define OTBR_SPINEL_HEADER_FLAG 0x80
#define OTBR_SPINEL_CMD_RESET 1
#define OTBR_SPINEL_CMD_PROP_VALUE_GET 2
#define OTBR_SPINEL_CMD_PROP_VALUE_IS 6
#define OTBR_SPINEL_RESET_STACK 2
#define OTBR_SPINEL_PROP_LAST_STATUS 0
#define OTBR_SPINEL_PROP_NCP_VERSION 2
#define OTBR_SPINEL_PROP_CAPS 5
#define OTBR_SPINEL_PROP_RADIO_CAPS 0x120B
#define OTBR_SPINEL_CAP_CONFIG_RADIO 34 // the RCP configuration
#define OTBR_SPINEL_STATUS_RESET_BEGIN 112
#define OTBR_SPINEL_STATUS_RESET_END 128

// otRadioCaps bits (openthread/platform/radio.h); br_core.c checks them against OpenThread's.
#define OTBR_RADIO_CAP_ACK_TIMEOUT (1u << 0)
#define OTBR_RADIO_CAP_TRANSMIT_RETRIES (1u << 2)
#define OTBR_RADIO_CAP_CSMA_BACKOFF (1u << 3)
#define OTBR_RADIO_CAP_TRANSMIT_SEC (1u << 5)
#define OTBR_RADIO_CAP_TRANSMIT_TIMING (1u << 6)
// What OpenThread requires from the RCP (Global Constraints: the ot-br-posix set for Thread 1.2+); the generated radio
// spinel file (tools/ot_radio_port.cmake) requires the same.
#define OTBR_RADIO_CAPS_REQUIRED (OTBR_RADIO_CAP_ACK_TIMEOUT | OTBR_RADIO_CAP_TRANSMIT_RETRIES | \
                                  OTBR_RADIO_CAP_CSMA_BACKOFF | OTBR_RADIO_CAP_TRANSMIT_SEC | \
                                  OTBR_RADIO_CAP_TRANSMIT_TIMING)

#define OTBR_PROBE_FRAME_MAX 8

// A spinel frame: the header's TID, the command, the property and its value.
typedef struct {
    uint8_t tid;
    uint32_t cmd;
    uint32_t key;
    const uint8_t *value;
    size_t value_len;
} otbr_spinel_msg_t;

// CMD_RESET (stack), TID 0. Returns its length (3).
size_t otbr_probe_reset_frame(uint8_t out[OTBR_PROBE_FRAME_MAX]);
// PROP_VALUE_GET of `prop` with TID 1..15. Returns its length; 0 for a TID out of range.
size_t otbr_probe_get_frame(uint8_t tid, uint32_t prop, uint8_t out[OTBR_PROBE_FRAME_MAX]);
// false for anything that is not a spinel frame with a command and a property (header flag, packed integers).
bool otbr_probe_parse(const uint8_t *frame, size_t len, otbr_spinel_msg_t *out);
// LAST_STATUS RESET_* with TID 0: the RCP (re)started.
bool otbr_probe_is_reset(const otbr_spinel_msg_t *m);
// PROP_VALUE_IS of `key` answering TID `tid`.
bool otbr_probe_is_answer(const otbr_spinel_msg_t *m, uint8_t tid, uint32_t key);
// Whether a CAPS value (packed integers) lists `cap`.
bool otbr_probe_has_cap(const uint8_t *value, size_t len, uint32_t cap);
// A RADIO_CAPS value (one packed integer). false when it does not parse.
bool otbr_probe_radio_caps(const uint8_t *value, size_t len, uint32_t *caps);
// The required caps a radio lacks, as "missing radio caps: ack-timeout, tx-timing"; "" when none. Returns the mask.
uint32_t otbr_probe_missing_caps(uint32_t caps, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
