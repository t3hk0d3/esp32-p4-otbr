#include "usbip_proto.h"

#include <string.h>

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

static uint32_t get32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void put_op_header(uint8_t *p, uint16_t code, uint32_t status)
{
    put16(p, USBIP_VERSION);
    put16(p + 2, code);
    put32(p + 4, status);
}

// The 312-byte device record; the interface records are separate.
static void put_dev_rec(uint8_t *p, const usbip_dev_rec_t *dev)
{
    memset(p, 0, USBIP_DEV_REC_LEN);
    strncpy((char *)p, dev->path, USBIP_PATH_LEN - 1);
    strncpy((char *)p + USBIP_PATH_LEN, dev->busid, USBIP_BUSID_LEN - 1);
    put32(p + 288, dev->busnum);
    put32(p + 292, dev->devnum);
    put32(p + 296, dev->speed);
    put16(p + 300, dev->vid);
    put16(p + 302, dev->pid);
    put16(p + 304, dev->bcd_device);
    p[306] = dev->dev_class;
    p[307] = dev->dev_subclass;
    p[308] = dev->dev_proto;
    p[309] = dev->config_value;
    p[310] = dev->num_configs;
    p[311] = dev->num_ifaces;
}

esp_err_t usbip_proto_parse_op_header(const uint8_t hdr[USBIP_OP_HDR_LEN], uint16_t *code)
{
    uint16_t c;

    if (get16(hdr) != USBIP_VERSION) {
        return ESP_ERR_INVALID_VERSION;
    }
    c = get16(hdr + 2);
    if (c != USBIP_OP_REQ_DEVLIST && c != USBIP_OP_REQ_IMPORT) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    *code = c;
    return ESP_OK;
}

size_t usbip_proto_encode_devlist(const usbip_dev_rec_t *dev, uint8_t *buf, size_t cap)
{
    size_t ifaces = 0, len = USBIP_OP_HDR_LEN + 4;

    if (dev != NULL) {
        ifaces = dev->num_ifaces < USBIP_IFACES_MAX ? dev->num_ifaces : USBIP_IFACES_MAX;
        len += USBIP_DEV_REC_LEN + ifaces * USBIP_IFACE_REC_LEN;
    }
    if (cap < len) {
        return 0;
    }
    put_op_header(buf, USBIP_OP_REP_DEVLIST, 0);
    put32(buf + USBIP_OP_HDR_LEN, dev != NULL ? 1 : 0);
    if (dev != NULL) {
        uint8_t *p = buf + USBIP_OP_HDR_LEN + 4;

        put_dev_rec(p, dev);
        p[311] = (uint8_t)ifaces;
        p += USBIP_DEV_REC_LEN;
        for (size_t i = 0; i < ifaces; i++, p += USBIP_IFACE_REC_LEN) {
            p[0] = dev->ifaces[i].cls;
            p[1] = dev->ifaces[i].subcls;
            p[2] = dev->ifaces[i].proto;
            p[3] = 0;
        }
    }
    return len;
}

size_t usbip_proto_encode_import_reply(uint32_t status, const usbip_dev_rec_t *dev, uint8_t *buf, size_t cap)
{
    size_t len = USBIP_OP_HDR_LEN;
    bool with_dev = status == 0 && dev != NULL;

    if (with_dev) {
        len += USBIP_DEV_REC_LEN;
    }
    if (cap < len) {
        return 0;
    }
    put_op_header(buf, USBIP_OP_REP_IMPORT, status);
    if (with_dev) {
        put_dev_rec(buf + USBIP_OP_HDR_LEN, dev);
    }
    return len;
}

esp_err_t usbip_proto_parse_cmd(const uint8_t hdr[USBIP_CMD_HDR_LEN], usbip_cmd_t *out)
{
    uint32_t command = get32(hdr);

    if (command != USBIP_CMD_SUBMIT && command != USBIP_CMD_UNLINK) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    out->command = command;
    out->seqnum = get32(hdr + 4);
    out->devid = get32(hdr + 8);
    out->direction = get32(hdr + 12);
    out->ep = get32(hdr + 16);
    if (command == USBIP_CMD_SUBMIT) {
        out->submit.flags = get32(hdr + 20);
        out->submit.length = (int32_t)get32(hdr + 24);
        out->submit.start_frame = (int32_t)get32(hdr + 28);
        out->submit.num_packets = (int32_t)get32(hdr + 32);
        out->submit.interval = (int32_t)get32(hdr + 36);
        memcpy(out->submit.setup, hdr + 40, sizeof(out->submit.setup));
    } else {
        out->unlink.unlink_seqnum = get32(hdr + 20);
    }
    return ESP_OK;
}

void usbip_proto_encode_ret_submit(const usbip_ret_submit_t *r, uint8_t hdr[USBIP_CMD_HDR_LEN])
{
    memset(hdr, 0, USBIP_CMD_HDR_LEN);
    put32(hdr, USBIP_RET_SUBMIT);
    put32(hdr + 4, r->seqnum);
    put32(hdr + 20, (uint32_t)r->status);
    put32(hdr + 24, (uint32_t)r->actual_length);
    put32(hdr + 28, (uint32_t)r->start_frame);
    put32(hdr + 32, (uint32_t)r->num_packets);
    put32(hdr + 36, (uint32_t)r->error_count);
}

void usbip_proto_encode_ret_unlink(uint32_t seqnum, int32_t status, uint8_t hdr[USBIP_CMD_HDR_LEN])
{
    memset(hdr, 0, USBIP_CMD_HDR_LEN);
    put32(hdr, USBIP_RET_UNLINK);
    put32(hdr + 4, seqnum);
    put32(hdr + 20, (uint32_t)status);
}

bool usbip_proto_first_byte_ok(uint8_t b)
{
    return b == (USBIP_VERSION >> 8);
}
