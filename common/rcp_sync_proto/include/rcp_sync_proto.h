#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RCP_SYNC_PROTO_MAGIC            0x5253
#define RCP_SYNC_PROTO_VERSION          1
#define RCP_SYNC_PROTO_MAX_PAYLOAD      4096
#define RCP_SYNC_PROTO_HEADER_LEN       6
#define RCP_SYNC_PROTO_CRC_LEN          4
#define RCP_SYNC_PROTO_VERSION_STR_LEN  32

/* Wire size of the HELLO reply payload: u16 + 3 * char[32] + u32. */
#define RCP_SYNC_PROTO_HELLO_REPLY_LEN  (2 + 3 * RCP_SYNC_PROTO_VERSION_STR_LEN + 4)

typedef enum {
    RCP_SYNC_MSG_HELLO = 1,
    RCP_SYNC_MSG_BEGIN,
    RCP_SYNC_MSG_DATA,
    RCP_SYNC_MSG_END,
    RCP_SYNC_MSG_OK = 0x80,
    RCP_SYNC_MSG_ERR,
    RCP_SYNC_MSG_NAK,
} rcp_sync_msg_t;

typedef enum {
    RCP_SYNC_ERR_SIZE = 1,
    RCP_SYNC_ERR_OFFSET,
    RCP_SYNC_ERR_STATE,
    RCP_SYNC_ERR_FLASH,
    RCP_SYNC_ERR_VERIFY,
} rcp_sync_err_code_t;

typedef struct {
    uint8_t type;
    uint8_t seq;
    uint16_t len;
    const uint8_t *payload;
} rcp_sync_proto_frame_t;

typedef struct {
    uint16_t proto_version;
    char updater_version[RCP_SYNC_PROTO_VERSION_STR_LEN];
    char boot_slot_version[RCP_SYNC_PROTO_VERSION_STR_LEN];
    char other_slot_version[RCP_SYNC_PROTO_VERSION_STR_LEN];
    uint32_t slot_size;
} rcp_sync_hello_reply_t;

/**
 * Serialise a frame (header, payload, CRC-32) into buf.
 * ESP_ERR_INVALID_ARG on NULL args or payload NULL with len > 0,
 * ESP_ERR_INVALID_SIZE if len > MAX_PAYLOAD or cap is too small.
 */
esp_err_t rcp_sync_proto_encode(const rcp_sync_proto_frame_t *frame, uint8_t *buf, size_t cap, size_t *out_len);

/**
 * Parse and verify one frame. out->payload points into buf.
 * ESP_ERR_INVALID_SIZE if buf is shorter than header + len + crc (or len > MAX_PAYLOAD),
 * ESP_ERR_INVALID_RESPONSE on wrong magic, ESP_ERR_INVALID_CRC on CRC mismatch.
 * Trailing bytes after the frame are ignored.
 */
esp_err_t rcp_sync_proto_decode(const uint8_t *buf, size_t len, rcp_sync_proto_frame_t *out);

/** Serialise a HELLO reply payload (RCP_SYNC_PROTO_HELLO_REPLY_LEN bytes). Strings are NUL-terminated on the wire. */
esp_err_t rcp_sync_proto_put_hello_reply(const rcp_sync_hello_reply_t *reply, uint8_t *buf, size_t cap, size_t *out_len);

/** Parse a HELLO reply payload; version strings in out are always NUL-terminated. */
esp_err_t rcp_sync_proto_get_hello_reply(const uint8_t *buf, size_t len, rcp_sync_hello_reply_t *out);

#ifdef __cplusplus
}
#endif
