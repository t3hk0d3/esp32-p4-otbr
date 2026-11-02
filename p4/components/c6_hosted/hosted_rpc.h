#pragma once

// The three ESP-Hosted RPCs the install needs, as hand-written protobuf (esp_hosted_rpc.proto, same ids from 0.0.6 to 2.x):
// message Rpc { RpcType msg_type = 1; RpcId msg_id = 2; uint32 uid = 3; oneof payload { ... = <msg id> } }.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HOSTED_RPC_TYPE_REQ   1
#define HOSTED_RPC_TYPE_RESP  2
#define HOSTED_RPC_TYPE_EVENT 3

#define HOSTED_RPC_REQ_OTA_BEGIN 272
#define HOSTED_RPC_REQ_OTA_WRITE 273
#define HOSTED_RPC_REQ_OTA_END   274
#define HOSTED_RPC_RESP_OFFSET   256 /* response id = request id + 256 */

/** Largest OTAWrite chunk: keeps header, serial TLV and protobuf inside one 1536-byte packet. */
#define HOSTED_RPC_OTA_CHUNK 1400

/** Room an encoded request needs at most (the OTAWrite one). */
#define HOSTED_RPC_REQ_MAX (HOSTED_RPC_OTA_CHUNK + 32)

/**
 * Encode a request: msg_type Req, msg_id, uid and the oneof payload (field number = msg_id). For OTAWrite the payload is
 * Req_OTAWrite { bytes ota_data = 1 } with `data`; OTABegin and OTAEnd carry an empty message (data must be NULL).
 * Returns the length, 0 on a bad argument or if it does not fit `cap`.
 */
size_t hosted_rpc_encode_req(uint32_t msg_id, uint32_t uid, const uint8_t *data, size_t len, uint8_t *out, size_t cap);

typedef struct {
    uint32_t msg_type;
    uint32_t msg_id;
    uint32_t uid;
    int32_t resp; /**< Resp_*.resp (field 1); 0 when absent (proto3 default) */
} hosted_rpc_msg_t;

/** Decode an Rpc message. Unknown fields are skipped. ESP_ERR_INVALID_RESPONSE on malformed protobuf. */
esp_err_t hosted_rpc_decode(const uint8_t *buf, size_t len, hosted_rpc_msg_t *out);

#ifdef __cplusplus
}
#endif
