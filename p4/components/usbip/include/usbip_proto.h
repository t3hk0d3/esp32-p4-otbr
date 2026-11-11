// USB/IP wire codec (protocol 1.1.1, Linux Documentation/usb/usbip_protocol.rst). Pure: explicit big-endian packing
// and parsing, no struct overlays, no I/O. The session engine and the socket glue sit on top of it.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define USBIP_PORT 3240
#define USBIP_VERSION 0x0111

#define USBIP_OP_REQ_DEVLIST 0x8005
#define USBIP_OP_REP_DEVLIST 0x0005
#define USBIP_OP_REQ_IMPORT 0x8003
#define USBIP_OP_REP_IMPORT 0x0003

#define USBIP_CMD_SUBMIT 1
#define USBIP_CMD_UNLINK 2
#define USBIP_RET_SUBMIT 3
#define USBIP_RET_UNLINK 4

#define USBIP_OP_HDR_LEN 8
#define USBIP_CMD_HDR_LEN 48
#define USBIP_DEV_REC_LEN 312
#define USBIP_IFACE_REC_LEN 4
#define USBIP_BUSID_LEN 32
#define USBIP_PATH_LEN 256
#define USBIP_IFACES_MAX 32

#define USBIP_URB_SHORT_NOT_OK 0x1
#define USBIP_URB_ZERO_PACKET 0x40

#define USBIP_DIR_OUT 0
#define USBIP_DIR_IN 1

typedef struct {
    uint8_t cls, subcls, proto;
} usbip_iface_rec_t;

typedef struct {
    char path[USBIP_PATH_LEN];
    char busid[USBIP_BUSID_LEN];
    uint32_t busnum, devnum, speed;
    uint16_t vid, pid, bcd_device;
    uint8_t dev_class, dev_subclass, dev_proto, config_value, num_configs, num_ifaces;
    usbip_iface_rec_t ifaces[USBIP_IFACES_MAX];
} usbip_dev_rec_t;

typedef struct {
    uint32_t command, seqnum, devid, direction, ep;
    union {
        struct {
            uint32_t flags;
            int32_t length, start_frame, num_packets, interval;
            uint8_t setup[8];
        } submit;
        struct {
            uint32_t unlink_seqnum;
        } unlink;
    };
} usbip_cmd_t;

typedef struct {
    uint32_t seqnum;
    int32_t status, actual_length, start_frame, num_packets, error_count;
} usbip_ret_submit_t;

// Checks the version and the code of an 8-byte operation header. ESP_ERR_INVALID_VERSION on another version,
// ESP_ERR_NOT_SUPPORTED on a code that is not REQ_DEVLIST or REQ_IMPORT; *code is set only on ESP_OK.
esp_err_t usbip_proto_parse_op_header(const uint8_t hdr[USBIP_OP_HDR_LEN], uint16_t *code);

// REP_DEVLIST: 12 bytes, then (with a device) its 312-byte record and 4 bytes per interface (num_ifaces, capped at
// USBIP_IFACES_MAX). dev NULL: zero devices. Returns the length written, 0 when cap is too small.
size_t usbip_proto_encode_devlist(const usbip_dev_rec_t *dev, uint8_t *buf, size_t cap);

// REP_IMPORT: 8 bytes, then (status 0 only) the 312-byte record. Returns the length written, 0 when cap is too small.
size_t usbip_proto_encode_import_reply(uint32_t status, const usbip_dev_rec_t *dev, uint8_t *buf, size_t cap);

// Parses a 48-byte CMD_SUBMIT or CMD_UNLINK header. ESP_ERR_INVALID_ARG on another command.
esp_err_t usbip_proto_parse_cmd(const uint8_t hdr[USBIP_CMD_HDR_LEN], usbip_cmd_t *out);

void usbip_proto_encode_ret_submit(const usbip_ret_submit_t *r, uint8_t hdr[USBIP_CMD_HDR_LEN]);
void usbip_proto_encode_ret_unlink(uint32_t seqnum, int32_t status, uint8_t hdr[USBIP_CMD_HDR_LEN]);

// The first byte of a connection must be the high byte of the version (0x01); anything else is not USB/IP.
bool usbip_proto_first_byte_ok(uint8_t b);

#ifdef __cplusplus
}
#endif
