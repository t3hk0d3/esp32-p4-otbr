#pragma once

// ESP-Hosted transport framing, pure (esp-hosted-mcu common/include/adapter.h, unchanged from 0.0.6 to 2.x):
// a 12-byte little-endian payload header in front of every packet, the serial TLV around RPC protobuf, and the PRIV
// startup event.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HOSTED_HEADER_LEN     12
#define HOSTED_MAX_PACKET     1536 /* MAX_SDIO_BUFFER_SIZE: header included */
#define HOSTED_IF_SERIAL      3
#define HOSTED_IF_PRIV        5
#define HOSTED_PRIV_PKT_EVENT 0x33
#define HOSTED_PRIV_EVENT_INIT 0x22
#define HOSTED_PRIV_TAG_CHIP_ID 0x12
#define HOSTED_CHIP_ID_ESP32C6 0x0D

/** Serial TLV overhead: type, length, endpoint name "RPCRsp", type, length. */
#define HOSTED_SERIAL_OVERHEAD (1 + 2 + 6 + 1 + 2)

typedef struct {
    uint8_t if_type;
    uint8_t if_num;
    uint8_t flags;
    uint8_t pkt_type; /* priv_pkt_type for PRIV */
    uint16_t seq;
    const uint8_t *payload;
    uint16_t len;
} hosted_packet_t;

/**
 * Header plus payload into out: offset 12, the checksum (u16 sum of all bytes with the checksum field zero) filled in.
 * Returns the packet length, 0 if it does not fit `cap` or HOSTED_MAX_PACKET.
 */
size_t hosted_frame_encode(uint8_t if_type, uint16_t seq, const uint8_t *payload, uint16_t len, uint8_t *out, size_t cap);

/**
 * Parse the packet at the start of a received stream. Returns the bytes it takes (header, any gap up to offset, payload)
 * and fills *out; 0 when buf holds less than one whole packet; -1 when the header is malformed (offset below 12 or a
 * packet longer than HOSTED_MAX_PACKET), after which the rest of the stream cannot be framed. A header of all zero bytes
 * (block padding) also returns -1.
 */
int hosted_frame_next(const uint8_t *buf, size_t len, hosted_packet_t *out);

/** Wrap RPC protobuf bytes in the serial TLV (endpoint "RPCRsp"). Returns the length, 0 if it does not fit. */
size_t hosted_serial_wrap(const uint8_t *proto, size_t len, uint8_t *out, size_t cap);

/**
 * Unwrap a serial TLV from the C6: endpoint "RPCRsp" (responses) or "RPCEvt" (events). ESP_ERR_INVALID_RESPONSE for
 * anything else or a length running past `len`.
 */
esp_err_t hosted_serial_unwrap(const uint8_t *buf, size_t len, bool *is_event, const uint8_t **proto, size_t *proto_len);

/**
 * The PRIV startup event (pkt_type HOSTED_PRIV_PKT_EVENT, event HOSTED_PRIV_EVENT_INIT): *chip_id gets the value of its
 * chip ID TLV. ESP_ERR_NOT_FOUND for another PRIV packet or an init event without that TLV.
 */
esp_err_t hosted_priv_init_chip_id(const hosted_packet_t *pkt, uint8_t *chip_id);

#ifdef __cplusplus
}
#endif
