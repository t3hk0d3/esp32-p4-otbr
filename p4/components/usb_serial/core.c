// Decisions of the USB serial proxy. Pure: no ESP-IDF calls.
#include "usb_serial_core.h"

#include <string.h>

#define PORT_WEB 80
#define PORT_BGP 179
#define BAUD_CLOSE_PERCENT 3

#define CDC_CLASS_COMM 0x02
#define CDC_CLASS_DATA 0x0A

#define VID_SILABS 0x10C4
#define VID_WCH 0x1A86

#define CP210X_FLOW_LIMIT 128
#define CP210X_HANDSHAKE_OFF 0x00000001
#define CP210X_HANDSHAKE_ON 0x00000009
#define CP210X_FLOW_REPLACE_OFF 0x00000040
#define CP210X_FLOW_REPLACE_ON 0x00000080

static const uint32_t accepted_bauds[] = {38400, 57600, 115200, 230400, 460800, 921600, 1000000, 2000000};

void usb_serial_settings_default(usb_serial_settings_t *out)
{
    out->enabled = false;
    out->port = USB_SERIAL_DEFAULT_PORT;
    out->baud = USB_SERIAL_DEFAULT_BAUD;
    out->flow_control = false;
}

usb_serial_check_t usb_serial_settings_check(const usb_serial_settings_t *s)
{
    if (s->port == 0 || s->port == PORT_WEB || s->port == PORT_BGP) {
        return USB_SERIAL_BAD_PORT;
    }
    for (size_t i = 0; i < sizeof(accepted_bauds) / sizeof(accepted_bauds[0]); i++) {
        if (s->baud == accepted_bauds[i]) {
            return USB_SERIAL_OK;
        }
    }
    return USB_SERIAL_BAD_BAUD;
}

usb_serial_driver_t usb_serial_pick_driver(uint16_t vid, uint16_t pid, uint8_t dev_class,
                                           const uint8_t *intf_classes, size_t n_intf)
{
    bool has_data = false;
    bool has_comm = (dev_class == CDC_CLASS_COMM);

    if (vid == VID_SILABS && (pid == 0xEA60 || pid == 0xEA70 || pid == 0xEA71)) {
        return USB_SERIAL_DRV_CP210X;
    }
    if (vid == VID_WCH && (pid == 0x7522 || pid == 0x7523 || pid == 0x5523)) {
        return USB_SERIAL_DRV_CH34X;
    }
    for (size_t i = 0; intf_classes != NULL && i < n_intf; i++) {
        if (intf_classes[i] == CDC_CLASS_DATA) {
            has_data = true;
        } else if (intf_classes[i] == CDC_CLASS_COMM) {
            has_comm = true;
        }
    }
    return (has_data && has_comm) ? USB_SERIAL_DRV_CDC_ACM : USB_SERIAL_DRV_NONE;
}

const char *usb_serial_driver_name(usb_serial_driver_t d)
{
    switch (d) {
    case USB_SERIAL_DRV_CP210X:
        return "cp210x";
    case USB_SERIAL_DRV_CH34X:
        return "ch34x";
    case USB_SERIAL_DRV_CDC_ACM:
        return "cdc_acm";
    case USB_SERIAL_DRV_NONE:
    default:
        return NULL;
    }
}

usb_serial_accept_t usb_serial_accept(bool client_connected)
{
    return client_connected ? USB_SERIAL_REFUSE : USB_SERIAL_TAKE;
}

void usb_serial_ring_init(usb_serial_ring_t *r, uint8_t *storage, size_t cap)
{
    r->data = storage;
    r->cap = cap;
    r->head = 0;
    r->len = 0;
}

bool usb_serial_ring_push(usb_serial_ring_t *r, const uint8_t *src, size_t n)
{
    if (n > r->cap - r->len) {
        return false;
    }
    size_t tail = (r->head + r->len) % r->cap;
    size_t first = r->cap - tail;

    if (first > n) {
        first = n;
    }
    memcpy(&r->data[tail], src, first);
    memcpy(r->data, src + first, n - first);
    r->len += n;
    return true;
}

size_t usb_serial_ring_take(usb_serial_ring_t *r, uint8_t *out, size_t cap)
{
    size_t n = r->len < cap ? r->len : cap;
    size_t first = r->cap - r->head;

    if (first > n) {
        first = n;
    }
    memcpy(out, &r->data[r->head], first);
    memcpy(out + first, r->data, n - first);
    r->head = (r->head + n) % r->cap;
    r->len -= n;
    return n;
}

void usb_serial_ring_clear(usb_serial_ring_t *r)
{
    r->head = 0;
    r->len = 0;
}

usb_serial_baud_t usb_serial_baud_verdict(uint32_t requested, uint32_t actual)
{
    if (actual == requested) {
        return USB_SERIAL_BAUD_EXACT;
    }
    if (actual == 0) {
        return USB_SERIAL_BAUD_FAR;
    }
    uint64_t diff = actual > requested ? actual - requested : requested - actual;

    return diff * 100 <= (uint64_t)requested * BAUD_CLOSE_PERCENT ? USB_SERIAL_BAUD_CLOSE : USB_SERIAL_BAUD_FAR;
}

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

void usb_serial_cp210x_flow(bool on, uint8_t out[USB_SERIAL_CP210X_FLOW_LEN])
{
    put_le32(&out[0], on ? CP210X_HANDSHAKE_ON : CP210X_HANDSHAKE_OFF);
    put_le32(&out[4], on ? CP210X_FLOW_REPLACE_ON : CP210X_FLOW_REPLACE_OFF);
    put_le32(&out[8], CP210X_FLOW_LIMIT);
    put_le32(&out[12], CP210X_FLOW_LIMIT);
}
