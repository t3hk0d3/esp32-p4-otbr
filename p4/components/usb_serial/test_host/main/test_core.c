#include <stdlib.h>
#include <string.h>
#include "unity.h"
#include "unity_test_runner.h"
#include "usb_serial_core.h"

#define RING_CAP USB_SERIAL_RING_MIN

static const uint32_t ACCEPTED_BAUDS[] = {38400, 57600, 115200, 230400, 460800, 921600, 1000000, 2000000};

static usb_serial_settings_t defaults(void)
{
    usb_serial_settings_t s;

    usb_serial_settings_default(&s);
    return s;
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ---- usb_serial_settings_default ---- */

TEST_CASE("On a call: gives enabled false, port 6638, baud 115200, flow_control false", "[usb_serial]")
{
    usb_serial_settings_t s = {.enabled = true, .port = 1, .baud = 1, .flow_control = true};

    usb_serial_settings_default(&s);
    TEST_ASSERT_FALSE(s.enabled);
    TEST_ASSERT_EQUAL_UINT16(6638, s.port);
    TEST_ASSERT_EQUAL_UINT32(115200, s.baud);
    TEST_ASSERT_FALSE(s.flow_control);
}

/* ---- usb_serial_settings_check ---- */

TEST_CASE("On the defaults: returns USB_SERIAL_OK", "[usb_serial]")
{
    usb_serial_settings_t s = defaults();

    TEST_ASSERT_EQUAL(USB_SERIAL_OK, usb_serial_settings_check(&s));
}

TEST_CASE("On each accepted baud rate: returns USB_SERIAL_OK", "[usb_serial]")
{
    for (size_t i = 0; i < sizeof(ACCEPTED_BAUDS) / sizeof(ACCEPTED_BAUDS[0]); i++) {
        usb_serial_settings_t s = defaults();

        s.baud = ACCEPTED_BAUDS[i];
        TEST_ASSERT_EQUAL_MESSAGE(USB_SERIAL_OK, usb_serial_settings_check(&s), "accepted baud rate refused");
    }
}

TEST_CASE("On a baud rate not in the list (9600, 115201, 3000000): returns USB_SERIAL_BAD_BAUD", "[usb_serial]")
{
    const uint32_t bad[] = {9600, 115201, 3000000};

    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        usb_serial_settings_t s = defaults();

        s.baud = bad[i];
        TEST_ASSERT_EQUAL(USB_SERIAL_BAD_BAUD, usb_serial_settings_check(&s));
    }
}

TEST_CASE("On port 0, 80 or 179: returns USB_SERIAL_BAD_PORT", "[usb_serial]")
{
    const uint16_t bad[] = {0, 80, 179};

    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        usb_serial_settings_t s = defaults();

        s.port = bad[i];
        TEST_ASSERT_EQUAL(USB_SERIAL_BAD_PORT, usb_serial_settings_check(&s));
    }
}

TEST_CASE("On port 1 and port 65535: returns USB_SERIAL_OK", "[usb_serial]")
{
    usb_serial_settings_t s = defaults();

    s.port = 1;
    TEST_ASSERT_EQUAL(USB_SERIAL_OK, usb_serial_settings_check(&s));
    s.port = 65535;
    TEST_ASSERT_EQUAL(USB_SERIAL_OK, usb_serial_settings_check(&s));
}

/* ---- usb_serial_pick_driver ---- */

TEST_CASE("On 10c4:ea60, 10c4:ea70, 10c4:ea71: returns USB_SERIAL_DRV_CP210X", "[usb_serial]")
{
    const uint8_t intf[] = {0xFF};

    TEST_ASSERT_EQUAL(USB_SERIAL_DRV_CP210X, usb_serial_pick_driver(0x10C4, 0xEA60, 0x00, intf, 1));
    TEST_ASSERT_EQUAL(USB_SERIAL_DRV_CP210X, usb_serial_pick_driver(0x10C4, 0xEA70, 0x00, intf, 1));
    TEST_ASSERT_EQUAL(USB_SERIAL_DRV_CP210X, usb_serial_pick_driver(0x10C4, 0xEA71, 0x00, intf, 1));
}

TEST_CASE("On 1a86:7522, 1a86:7523, 1a86:5523: returns USB_SERIAL_DRV_CH34X", "[usb_serial]")
{
    const uint8_t intf[] = {0xFF};

    TEST_ASSERT_EQUAL(USB_SERIAL_DRV_CH34X, usb_serial_pick_driver(0x1A86, 0x7522, 0xFF, intf, 1));
    TEST_ASSERT_EQUAL(USB_SERIAL_DRV_CH34X, usb_serial_pick_driver(0x1A86, 0x7523, 0xFF, intf, 1));
    TEST_ASSERT_EQUAL(USB_SERIAL_DRV_CH34X, usb_serial_pick_driver(0x1A86, 0x5523, 0xFF, intf, 1));
}

TEST_CASE("On a device with interfaces of class 0x02 and 0x0A (any VID): returns USB_SERIAL_DRV_CDC_ACM",
          "[usb_serial]")
{
    const uint8_t intf[] = {0x02, 0x0A};

    TEST_ASSERT_EQUAL(USB_SERIAL_DRV_CDC_ACM, usb_serial_pick_driver(0x303A, 0x1001, 0x00, intf, 2));
    TEST_ASSERT_EQUAL(USB_SERIAL_DRV_CDC_ACM, usb_serial_pick_driver(0x1234, 0x5678, 0xEF, intf, 2));
}

TEST_CASE("On a device of class 0x02 with one interface of class 0x0A: returns USB_SERIAL_DRV_CDC_ACM",
          "[usb_serial]")
{
    const uint8_t intf[] = {0x0A};

    TEST_ASSERT_EQUAL(USB_SERIAL_DRV_CDC_ACM, usb_serial_pick_driver(0x2341, 0x0043, 0x02, intf, 1));
}

TEST_CASE("On an FTDI device (0403:6001, vendor class 0xFF): returns USB_SERIAL_DRV_NONE", "[usb_serial]")
{
    const uint8_t intf[] = {0xFF};

    TEST_ASSERT_EQUAL(USB_SERIAL_DRV_NONE, usb_serial_pick_driver(0x0403, 0x6001, 0xFF, intf, 1));
}

TEST_CASE("On a mass-storage device (interface class 0x08): returns USB_SERIAL_DRV_NONE", "[usb_serial]")
{
    const uint8_t intf[] = {0x08};

    TEST_ASSERT_EQUAL(USB_SERIAL_DRV_NONE, usb_serial_pick_driver(0x0781, 0x5567, 0x00, intf, 1));
}

TEST_CASE("On a device with only a CDC data interface and no communications class: returns USB_SERIAL_DRV_NONE",
          "[usb_serial]")
{
    const uint8_t intf[] = {0x0A};

    TEST_ASSERT_EQUAL(USB_SERIAL_DRV_NONE, usb_serial_pick_driver(0x1234, 0x5678, 0x00, intf, 1));
    TEST_ASSERT_EQUAL(USB_SERIAL_DRV_NONE, usb_serial_pick_driver(0x1234, 0x5678, 0x00, NULL, 0));
}

/* ---- usb_serial_driver_name ---- */

TEST_CASE("On each driver: returns \"cp210x\", \"ch34x\", \"cdc_acm\", or NULL for NONE", "[usb_serial]")
{
    TEST_ASSERT_EQUAL_STRING("cp210x", usb_serial_driver_name(USB_SERIAL_DRV_CP210X));
    TEST_ASSERT_EQUAL_STRING("ch34x", usb_serial_driver_name(USB_SERIAL_DRV_CH34X));
    TEST_ASSERT_EQUAL_STRING("cdc_acm", usb_serial_driver_name(USB_SERIAL_DRV_CDC_ACM));
    TEST_ASSERT_NULL(usb_serial_driver_name(USB_SERIAL_DRV_NONE));
}

/* ---- usb_serial_accept ---- */

TEST_CASE("On a connection with no client: returns USB_SERIAL_TAKE", "[usb_serial]")
{
    TEST_ASSERT_EQUAL(USB_SERIAL_TAKE, usb_serial_accept(false));
}

TEST_CASE("On a connection while a client is connected: returns USB_SERIAL_REFUSE", "[usb_serial]")
{
    TEST_ASSERT_EQUAL(USB_SERIAL_REFUSE, usb_serial_accept(true));
}

/* ---- usb_serial_ring ---- */

TEST_CASE("On pushes within the capacity: take returns the bytes in order, across the wrap point", "[usb_serial]")
{
    uint8_t storage[16];
    uint8_t out[16];
    usb_serial_ring_t r;
    uint8_t a[12], b[10];

    for (size_t i = 0; i < sizeof(a); i++) {
        a[i] = (uint8_t)(i + 1);
    }
    for (size_t i = 0; i < sizeof(b); i++) {
        b[i] = (uint8_t)(100 + i);
    }
    usb_serial_ring_init(&r, storage, sizeof(storage));
    TEST_ASSERT_TRUE(usb_serial_ring_push(&r, a, sizeof(a)));
    // Drain 8 so the head sits at 8 and the next push wraps.
    TEST_ASSERT_EQUAL(8, usb_serial_ring_take(&r, out, 8));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(a, out, 8);
    TEST_ASSERT_TRUE(usb_serial_ring_push(&r, b, sizeof(b)));
    TEST_ASSERT_EQUAL(14, r.len);
    TEST_ASSERT_EQUAL(14, usb_serial_ring_take(&r, out, sizeof(out)));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(&a[8], out, 4);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(b, &out[4], 10);
    TEST_ASSERT_EQUAL(0, r.len);
}

TEST_CASE("On a push that would overflow: the ring is unchanged", "[usb_serial]")
{
    static uint8_t storage[RING_CAP];
    static uint8_t fill[RING_CAP - 2];
    static uint8_t out[RING_CAP];
    const uint8_t extra[3] = {0xAA, 0xBB, 0xCC};
    usb_serial_ring_t r;

    for (size_t i = 0; i < sizeof(fill); i++) {
        fill[i] = (uint8_t)(i * 7 + 1);
    }
    usb_serial_ring_init(&r, storage, sizeof(storage));
    TEST_ASSERT_TRUE(usb_serial_ring_push(&r, fill, sizeof(fill)));
    TEST_ASSERT_EQUAL(8190, r.len);

    TEST_ASSERT_FALSE(usb_serial_ring_push(&r, extra, sizeof(extra)));
    TEST_ASSERT_EQUAL(8190, r.len);
    TEST_ASSERT_EQUAL(8190, usb_serial_ring_take(&r, out, sizeof(out)));
    TEST_ASSERT_EQUAL_UINT8(fill[0], out[0]);
    TEST_ASSERT_EQUAL_UINT8(fill[sizeof(fill) - 1], out[sizeof(fill) - 1]);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(fill, out, sizeof(fill));
}

TEST_CASE("On clear: leaves the ring empty", "[usb_serial]")
{
    uint8_t storage[16];
    uint8_t out[16];
    const uint8_t data[5] = {1, 2, 3, 4, 5};
    usb_serial_ring_t r;

    usb_serial_ring_init(&r, storage, sizeof(storage));
    TEST_ASSERT_TRUE(usb_serial_ring_push(&r, data, sizeof(data)));
    usb_serial_ring_clear(&r);
    TEST_ASSERT_EQUAL(0, r.len);
    TEST_ASSERT_EQUAL(0, usb_serial_ring_take(&r, out, sizeof(out)));
    // The cleared ring is fully usable again.
    TEST_ASSERT_TRUE(usb_serial_ring_push(&r, data, sizeof(data)));
    TEST_ASSERT_EQUAL(5, usb_serial_ring_take(&r, out, sizeof(out)));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(data, out, 5);
}

/* ---- usb_serial_baud_verdict ---- */

TEST_CASE("On actual == requested: returns USB_SERIAL_BAUD_EXACT", "[usb_serial]")
{
    TEST_ASSERT_EQUAL(USB_SERIAL_BAUD_EXACT, usb_serial_baud_verdict(115200, 115200));
}

TEST_CASE("On 921600 read back as 923076: returns USB_SERIAL_BAUD_CLOSE", "[usb_serial]")
{
    TEST_ASSERT_EQUAL(USB_SERIAL_BAUD_CLOSE, usb_serial_baud_verdict(921600, 923076));
}

TEST_CASE("On 115200 read back as 9600, and on actual 0: returns USB_SERIAL_BAUD_FAR", "[usb_serial]")
{
    TEST_ASSERT_EQUAL(USB_SERIAL_BAUD_FAR, usb_serial_baud_verdict(115200, 9600));
    TEST_ASSERT_EQUAL(USB_SERIAL_BAUD_FAR, usb_serial_baud_verdict(115200, 0));
}

/* ---- usb_serial_cp210x_flow ---- */

TEST_CASE("On flow control off: writes ulControlHandshake 0x00000001, ulFlowReplace 0x00000040, limits 128",
          "[usb_serial]")
{
    const uint8_t expected[USB_SERIAL_CP210X_FLOW_LEN] = {
        0x01, 0x00, 0x00, 0x00,
        0x40, 0x00, 0x00, 0x00,
        0x80, 0x00, 0x00, 0x00,
        0x80, 0x00, 0x00, 0x00,
    };
    uint8_t out[USB_SERIAL_CP210X_FLOW_LEN];

    memset(out, 0xEE, sizeof(out));
    usb_serial_cp210x_flow(false, out);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, out, sizeof(expected));
    TEST_ASSERT_EQUAL_UINT32(0x00000001, le32(&out[0]));
}

TEST_CASE("On flow control on: writes ulControlHandshake 0x00000009, ulFlowReplace 0x00000080, limits 128",
          "[usb_serial]")
{
    const uint8_t expected[USB_SERIAL_CP210X_FLOW_LEN] = {
        0x09, 0x00, 0x00, 0x00,
        0x80, 0x00, 0x00, 0x00,
        0x80, 0x00, 0x00, 0x00,
        0x80, 0x00, 0x00, 0x00,
    };
    uint8_t out[USB_SERIAL_CP210X_FLOW_LEN];

    memset(out, 0xEE, sizeof(out));
    usb_serial_cp210x_flow(true, out);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, out, sizeof(expected));
    TEST_ASSERT_EQUAL_UINT32(0x00000009, le32(&out[0]));
}

void app_main(void)
{
    UNITY_BEGIN();
    unity_run_all_tests();
    int failures = (int)UNITY_END();
    exit(failures == 0 ? 0 : 1);
}
