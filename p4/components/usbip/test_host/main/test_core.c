#include <string.h>

#include "unity.h"
#include "unity_test_runner.h"
#include "usbip_core.h"

#define W(n) ((n) & 0xff), (((n) >> 8) & 0xff)

static const uint8_t dev_desc_plain[18] = {
    18, 1, W(0x0200), 0, 0, 0, 64, W(0x10c4), W(0xea60), W(0x0100), 1, 2, 3, 1,
};

// CP2102N: one vendor interface, two bulk endpoints.
static const uint8_t cfg_cp2102n[] = {
    9, 2, W(32), 1, 1, 0, 0x80, 50,
    9, 4, 0, 0, 2, 0xff, 0, 0, 0,
    7, 5, 0x81, 2, W(64), 0,
    7, 5, 0x01, 2, W(64), 0,
};

// Bluetooth adapter: interface 0 has intr IN and bulk IN/OUT, interface 1 has two zero-MPS iso endpoints at alt 0
// and two real ones at alt 1.
static const uint8_t cfg_bt[] = {
    9, 2, W(85), 2, 1, 0, 0xe0, 50,
    9, 4, 0, 0, 3, 0xe0, 1, 1, 0,
    7, 5, 0x81, 3, W(16), 1,
    7, 5, 0x82, 2, W(64), 0,
    7, 5, 0x02, 2, W(64), 0,
    9, 4, 1, 0, 2, 0xe0, 1, 1, 0,
    7, 5, 0x83, 1, W(0), 1,
    7, 5, 0x03, 1, W(0), 1,
    9, 4, 1, 1, 2, 0xe0, 1, 1, 0,
    7, 5, 0x83, 1, W(9), 1,
    7, 5, 0x03, 1, W(9), 1,
};

static const uint8_t cfg_hub[] = {
    9, 2, W(25), 1, 1, 0, 0xe0, 50,
    9, 4, 0, 0, 1, 9, 0, 0, 0,
    7, 5, 0x81, 3, W(1), 12,
};

static const uint8_t dev_desc_hub[18] = {
    18, 1, W(0x0200), 9, 0, 1, 64, W(0x1d6b), W(0x0002), W(0x0100), 1, 2, 3, 1,
};

// One interface with n bulk IN endpoints (n up to 16).
static size_t make_cfg(uint8_t *buf, unsigned n)
{
    size_t total = 18 + 7 * n;
    size_t o = 0;

    const uint8_t head[] = {9, 2, W(total), 1, 1, 0, 0x80, 50, 9, 4, 0, 0, (uint8_t)n, 0xff, 0, 0, 0};

    memcpy(buf, head, sizeof(head));
    o = sizeof(head);
    for (unsigned i = 0; i < n; i++) {
        const uint8_t ep[] = {7, 5, (uint8_t)(0x81 + i), 2, W(64), 0};

        memcpy(buf + o, ep, sizeof(ep));
        o += sizeof(ep);
    }
    return o;
}

/* ---- usbip_core_dev_record ---- */

TEST_CASE("On a plain device: fills ids, class, speed, bConfigurationValue, interfaces from the raw descriptors",
          "[usbip]")
{
    usbip_dev_rec_t r;
    char why[64] = "";

    TEST_ASSERT_EQUAL(ESP_OK, usbip_core_dev_record(dev_desc_plain, cfg_cp2102n, sizeof(cfg_cp2102n), 5, 2, &r, why,
                                                    sizeof(why)));
    TEST_ASSERT_EQUAL_STRING(USBIP_PATH, r.path);
    TEST_ASSERT_EQUAL_STRING("1-1", r.busid);
    TEST_ASSERT_EQUAL_UINT32(1, r.busnum);
    TEST_ASSERT_EQUAL_UINT32(5, r.devnum);
    TEST_ASSERT_EQUAL_UINT32(2, r.speed);
    TEST_ASSERT_EQUAL_HEX16(0x10c4, r.vid);
    TEST_ASSERT_EQUAL_HEX16(0xea60, r.pid);
    TEST_ASSERT_EQUAL_HEX16(0x0100, r.bcd_device);
    TEST_ASSERT_EQUAL_UINT8(0, r.dev_class);
    TEST_ASSERT_EQUAL_UINT8(1, r.config_value);
    TEST_ASSERT_EQUAL_UINT8(1, r.num_configs);
    TEST_ASSERT_EQUAL_UINT8(1, r.num_ifaces);
    TEST_ASSERT_EQUAL_HEX8(0xff, r.ifaces[0].cls);
}

TEST_CASE("On a plain device with several interfaces: lists each interface once, from its alt 0", "[usbip]")
{
    usbip_dev_rec_t r;

    TEST_ASSERT_EQUAL(ESP_OK, usbip_core_dev_record(dev_desc_plain, cfg_bt, sizeof(cfg_bt), 2, 3, &r, NULL, 0));
    TEST_ASSERT_EQUAL_UINT8(2, r.num_ifaces);
    TEST_ASSERT_EQUAL_HEX8(0xe0, r.ifaces[1].cls);
    TEST_ASSERT_EQUAL_HEX8(1, r.ifaces[1].subcls);
    TEST_ASSERT_EQUAL_HEX8(1, r.ifaces[1].proto);
}

TEST_CASE("On a hub by device class: returns ESP_ERR_NOT_SUPPORTED, why \"hubs are not supported\"", "[usbip]")
{
    usbip_dev_rec_t r;
    char why[64] = "";

    TEST_ASSERT_EQUAL(ESP_ERR_NOT_SUPPORTED,
                      usbip_core_dev_record(dev_desc_hub, cfg_cp2102n, sizeof(cfg_cp2102n), 2, 2, &r, why,
                                            sizeof(why)));
    TEST_ASSERT_EQUAL_STRING("hubs are not supported", why);
}

TEST_CASE("On a hub by interface class: returns ESP_ERR_NOT_SUPPORTED, why \"hubs are not supported\"", "[usbip]")
{
    usbip_dev_rec_t r;
    char why[64] = "";

    TEST_ASSERT_EQUAL(ESP_ERR_NOT_SUPPORTED,
                      usbip_core_dev_record(dev_desc_plain, cfg_hub, sizeof(cfg_hub), 2, 2, &r, why, sizeof(why)));
    TEST_ASSERT_EQUAL_STRING("hubs are not supported", why);
}

TEST_CASE("On 15 endpoints over the interfaces' alt 0: returns ESP_OK", "[usbip]")
{
    uint8_t cfg[200];
    size_t len = make_cfg(cfg, 15);
    usbip_dev_rec_t r;

    TEST_ASSERT_EQUAL(ESP_OK, usbip_core_dev_record(dev_desc_plain, cfg, len, 2, 2, &r, NULL, 0));
}

TEST_CASE("On more than 15 endpoints over the interfaces' alt 0: returns ESP_ERR_NOT_SUPPORTED, why names the count",
          "[usbip]")
{
    uint8_t cfg[200];
    size_t len = make_cfg(cfg, 16);
    usbip_dev_rec_t r;
    char why[96] = "";

    TEST_ASSERT_EQUAL(ESP_ERR_NOT_SUPPORTED,
                      usbip_core_dev_record(dev_desc_plain, cfg, len, 2, 2, &r, why, sizeof(why)));
    TEST_ASSERT_NOT_NULL(strstr(why, "16"));
}

TEST_CASE("On a Bluetooth adapter: counts the zero-MPS iso endpoints at alt 0 (5 endpoints) and accepts it", "[usbip]")
{
    usbip_dev_rec_t r;

    TEST_ASSERT_EQUAL(ESP_OK, usbip_core_dev_record(dev_desc_plain, cfg_bt, sizeof(cfg_bt), 2, 3, &r, NULL, 0));
}

TEST_CASE("On a truncated configuration descriptor: returns ESP_ERR_INVALID_SIZE", "[usbip]")
{
    usbip_dev_rec_t r;

    // Cut in the middle of the last endpoint descriptor, and shorter than the header itself.
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE,
                      usbip_core_dev_record(dev_desc_plain, cfg_cp2102n, 29, 2, 2, &r, NULL, 0));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, usbip_core_dev_record(dev_desc_plain, cfg_cp2102n, 5, 2, 2, &r, NULL, 0));
}

TEST_CASE("On a descriptor with bLength 0: returns ESP_ERR_INVALID_SIZE", "[usbip]")
{
    uint8_t cfg[sizeof(cfg_cp2102n)];
    usbip_dev_rec_t r;

    memcpy(cfg, cfg_cp2102n, sizeof(cfg));
    cfg[18] = 0;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, usbip_core_dev_record(dev_desc_plain, cfg, sizeof(cfg), 2, 2, &r, NULL, 0));
}

/* ---- usbip_core_endpoints ---- */

TEST_CASE("On an interface's alternate setting: lists address, type and max packet size", "[usbip]")
{
    usbip_ep_info_t ep[4];

    TEST_ASSERT_EQUAL_UINT(3, usbip_core_endpoints(cfg_bt, sizeof(cfg_bt), 0, 0, ep, 4));
    TEST_ASSERT_EQUAL_HEX8(0x81, ep[0].addr);
    TEST_ASSERT_EQUAL_UINT8(3, ep[0].type);
    TEST_ASSERT_EQUAL_UINT16(16, ep[0].mps);
    TEST_ASSERT_EQUAL_HEX8(0x02, ep[2].addr);
    TEST_ASSERT_EQUAL_UINT8(2, ep[2].type);

    TEST_ASSERT_EQUAL_UINT(2, usbip_core_endpoints(cfg_bt, sizeof(cfg_bt), 1, 1, ep, 4));
    TEST_ASSERT_EQUAL_HEX8(0x83, ep[0].addr);
    TEST_ASSERT_EQUAL_UINT8(1, ep[0].type);
    TEST_ASSERT_EQUAL_UINT16(9, ep[0].mps);
}

TEST_CASE("On an interface's alternate setting with a small cap: stores cap entries, returns the full count",
          "[usbip]")
{
    usbip_ep_info_t ep[1];

    TEST_ASSERT_EQUAL_UINT(3, usbip_core_endpoints(cfg_bt, sizeof(cfg_bt), 0, 0, ep, 1));
    TEST_ASSERT_EQUAL_HEX8(0x81, ep[0].addr);
}

TEST_CASE("On an unknown interface or alt: lists nothing", "[usbip]")
{
    usbip_ep_info_t ep[4];

    TEST_ASSERT_EQUAL_UINT(0, usbip_core_endpoints(cfg_bt, sizeof(cfg_bt), 5, 0, ep, 4));
    TEST_ASSERT_EQUAL_UINT(0, usbip_core_endpoints(cfg_bt, sizeof(cfg_bt), 1, 7, ep, 4));
}

TEST_CASE("On a high-speed endpoint with additional transactions: mps keeps only the packet size bits", "[usbip]")
{
    const uint8_t cfg[] = {
        9, 2, W(25), 1, 1, 0, 0x80, 50,
        9, 4, 0, 0, 1, 0xff, 0, 0, 0,
        7, 5, 0x81, 3, W(0x1000 | 0x0800 | 64), 1,
    };
    usbip_ep_info_t ep[1];

    TEST_ASSERT_EQUAL_UINT(1, usbip_core_endpoints(cfg, sizeof(cfg), 0, 0, ep, 1));
    TEST_ASSERT_EQUAL_UINT16(64, ep[0].mps);
}

TEST_CASE("On a short endpoint descriptor: dev_record and endpoints both skip it", "[usbip]")
{
    // 16 bulk endpoints, the last one with bLength 6: only 15 count.
    uint8_t cfg[200];
    size_t len = make_cfg(cfg, 16);
    usbip_dev_rec_t r;
    usbip_ep_info_t ep[16];

    cfg[len - 7] = 6;
    len -= 1; // the shortened descriptor ends the configuration
    cfg[2] = (uint8_t)len;
    cfg[3] = 0;
    TEST_ASSERT_EQUAL(ESP_OK, usbip_core_dev_record(dev_desc_plain, cfg, len, 2, 2, &r, NULL, 0));
    TEST_ASSERT_EQUAL_UINT(15, usbip_core_endpoints(cfg, len, 0, 0, ep, 16));
}

/* ---- usbip_core_classify_setup ---- */

TEST_CASE("On SET_CONFIGURATION to the active value: USBIP_REQ_SET_CONFIG_SAME", "[usbip]")
{
    const uint8_t s[8] = {0x00, 9, 1, 0, 0, 0, 0, 0};

    TEST_ASSERT_EQUAL(USBIP_REQ_SET_CONFIG_SAME, usbip_core_classify_setup(s, 1));
}

TEST_CASE("On SET_CONFIGURATION to another value: USBIP_REQ_SET_CONFIG_OTHER", "[usbip]")
{
    const uint8_t s[8] = {0x00, 9, 2, 0, 0, 0, 0, 0};

    TEST_ASSERT_EQUAL(USBIP_REQ_SET_CONFIG_OTHER, usbip_core_classify_setup(s, 1));
}

TEST_CASE("On SET_INTERFACE: USBIP_REQ_SET_INTERFACE", "[usbip]")
{
    const uint8_t s[8] = {0x01, 11, 1, 0, 1, 0, 0, 0};

    TEST_ASSERT_EQUAL(USBIP_REQ_SET_INTERFACE, usbip_core_classify_setup(s, 1));
}

TEST_CASE("On CLEAR_FEATURE(ENDPOINT_HALT): USBIP_REQ_CLEAR_HALT", "[usbip]")
{
    const uint8_t s[8] = {0x02, 1, 0, 0, 0x81, 0, 0, 0};

    TEST_ASSERT_EQUAL(USBIP_REQ_CLEAR_HALT, usbip_core_classify_setup(s, 1));
}

TEST_CASE("On CLEAR_FEATURE of another feature: USBIP_REQ_FORWARD", "[usbip]")
{
    const uint8_t s[8] = {0x00, 1, 1, 0, 0, 0, 0, 0};

    TEST_ASSERT_EQUAL(USBIP_REQ_FORWARD, usbip_core_classify_setup(s, 1));
}

TEST_CASE("On SET_ADDRESS: USBIP_REQ_SET_ADDRESS", "[usbip]")
{
    const uint8_t s[8] = {0x00, 5, 7, 0, 0, 0, 0, 0};

    TEST_ASSERT_EQUAL(USBIP_REQ_SET_ADDRESS, usbip_core_classify_setup(s, 1));
}

TEST_CASE("On SET_FEATURE(PORT_RESET) to a port: USBIP_REQ_PORT_RESET", "[usbip]")
{
    const uint8_t s[8] = {0x23, 3, 4, 0, 1, 0, 0, 0};

    TEST_ASSERT_EQUAL(USBIP_REQ_PORT_RESET, usbip_core_classify_setup(s, 1));
}

TEST_CASE("On class and vendor requests: USBIP_REQ_FORWARD", "[usbip]")
{
    const uint8_t cls[8] = {0xa1, 1, 0, 0, 0, 0, 7, 0};
    const uint8_t vendor_set_cfg_number[8] = {0x40, 9, 1, 0, 0, 0, 0, 0};
    const uint8_t get_desc[8] = {0x80, 6, 0, 1, 0, 0, 18, 0};

    TEST_ASSERT_EQUAL(USBIP_REQ_FORWARD, usbip_core_classify_setup(cls, 1));
    TEST_ASSERT_EQUAL(USBIP_REQ_FORWARD, usbip_core_classify_setup(vendor_set_cfg_number, 1));
    TEST_ASSERT_EQUAL(USBIP_REQ_FORWARD, usbip_core_classify_setup(get_desc, 1));
}

/* ---- usbip_core_status_errno ---- */

TEST_CASE("On COMPLETED: 0, and -EREMOTEIO for a short IN with URB_SHORT_NOT_OK", "[usbip]")
{
    TEST_ASSERT_EQUAL_INT32(0, usbip_core_status_errno(USBIP_XS_COMPLETED, false, 0));
    TEST_ASSERT_EQUAL_INT32(0, usbip_core_status_errno(USBIP_XS_COMPLETED, true, 0));
    TEST_ASSERT_EQUAL_INT32(0, usbip_core_status_errno(USBIP_XS_COMPLETED, false, USBIP_URB_SHORT_NOT_OK));
    TEST_ASSERT_EQUAL_INT32(-121, usbip_core_status_errno(USBIP_XS_COMPLETED, true, USBIP_URB_SHORT_NOT_OK));
}

TEST_CASE("On a failed transfer: the Linux errno of the status", "[usbip]")
{
    TEST_ASSERT_EQUAL_INT32(-32, usbip_core_status_errno(USBIP_XS_STALL, false, 0));
    TEST_ASSERT_EQUAL_INT32(-75, usbip_core_status_errno(USBIP_XS_OVERFLOW, false, 0));
    TEST_ASSERT_EQUAL_INT32(-71, usbip_core_status_errno(USBIP_XS_ERROR, false, 0));
    TEST_ASSERT_EQUAL_INT32(-19, usbip_core_status_errno(USBIP_XS_NO_DEVICE, false, 0));
    TEST_ASSERT_EQUAL_INT32(-104, usbip_core_status_errno(USBIP_XS_CANCELED, false, 0));
}

/* ---- usbip_core_check_submit ---- */

static usbip_cmd_t submit_cmd(uint32_t devid, int32_t length)
{
    usbip_cmd_t c = {0};

    c.command = USBIP_CMD_SUBMIT;
    c.devid = devid;
    c.submit.length = length;
    return c;
}

TEST_CASE("On a transfer above 16384: -EINVAL, and 16384 itself passes", "[usbip]")
{
    const usbip_ep_info_t bulk = {.addr = 0x81, .type = 2, .mps = 64};
    usbip_cmd_t c = submit_cmd(0x10005, 16385);

    TEST_ASSERT_EQUAL_INT32(-22, usbip_core_check_submit(&c, 0x10005, &bulk));
    c.submit.length = 16384;
    TEST_ASSERT_EQUAL_INT32(0, usbip_core_check_submit(&c, 0x10005, &bulk));
    c.submit.length = -1;
    TEST_ASSERT_EQUAL_INT32(-22, usbip_core_check_submit(&c, 0x10005, &bulk));
}

TEST_CASE("On control data above 4096: -EINVAL, and 4096 itself passes", "[usbip]")
{
    const usbip_ep_info_t ep0 = {.addr = 0, .type = 0, .mps = 64};
    usbip_cmd_t c = submit_cmd(0x10005, 4097);

    TEST_ASSERT_EQUAL_INT32(-22, usbip_core_check_submit(&c, 0x10005, &ep0));
    c.submit.length = 4096;
    TEST_ASSERT_EQUAL_INT32(0, usbip_core_check_submit(&c, 0x10005, &ep0));
}

TEST_CASE("On an isochronous endpoint: -EINVAL", "[usbip]")
{
    const usbip_ep_info_t iso = {.addr = 0x83, .type = 1, .mps = 9};
    usbip_cmd_t c = submit_cmd(0x10005, 9);

    TEST_ASSERT_EQUAL_INT32(-22, usbip_core_check_submit(&c, 0x10005, &iso));
}

TEST_CASE("On an unclaimed endpoint: -EINVAL", "[usbip]")
{
    usbip_cmd_t c = submit_cmd(0x10005, 8);

    TEST_ASSERT_EQUAL_INT32(-22, usbip_core_check_submit(&c, 0x10005, NULL));
}

TEST_CASE("On the wrong devid: -ENODEV", "[usbip]")
{
    const usbip_ep_info_t bulk = {.addr = 0x81, .type = 2, .mps = 64};
    usbip_cmd_t c = submit_cmd(0x10006, 8);

    TEST_ASSERT_EQUAL_INT32(-19, usbip_core_check_submit(&c, 0x10005, &bulk));
}

TEST_CASE("On a valid bulk or interrupt submit: 0", "[usbip]")
{
    const usbip_ep_info_t bulk = {.addr = 0x01, .type = 2, .mps = 64};
    const usbip_ep_info_t intr = {.addr = 0x81, .type = 3, .mps = 16};
    usbip_cmd_t c = submit_cmd(0x10005, 512);

    TEST_ASSERT_EQUAL_INT32(0, usbip_core_check_submit(&c, 0x10005, &bulk));
    TEST_ASSERT_EQUAL_INT32(0, usbip_core_check_submit(&c, 0x10005, &intr));
}

/* ---- usbip_core_check_enable ---- */

TEST_CASE("On a request equal to the stored value: USBIP_SET_NOOP", "[usbip]")
{
    TEST_ASSERT_EQUAL(USBIP_SET_NOOP, usbip_core_check_enable(true, true, true, true));
    TEST_ASSERT_EQUAL(USBIP_SET_NOOP, usbip_core_check_enable(false, false, false, false));
}

TEST_CASE("On enable with a held USB stack: USBIP_SET_REFUSE_IN_USE", "[usbip]")
{
    TEST_ASSERT_EQUAL(USBIP_SET_REFUSE_IN_USE, usbip_core_check_enable(true, false, false, true));
    TEST_ASSERT_EQUAL(USBIP_SET_REFUSE_IN_USE, usbip_core_check_enable(true, false, true, true));
}

TEST_CASE("On enable during a long operation: USBIP_SET_REFUSE_BUSY", "[usbip]")
{
    TEST_ASSERT_EQUAL(USBIP_SET_REFUSE_BUSY, usbip_core_check_enable(true, false, true, false));
}

TEST_CASE("On enable otherwise, or disable: USBIP_SET_REBOOT", "[usbip]")
{
    TEST_ASSERT_EQUAL(USBIP_SET_REBOOT, usbip_core_check_enable(true, false, false, false));
    TEST_ASSERT_EQUAL(USBIP_SET_REBOOT, usbip_core_check_enable(false, true, false, false));
    TEST_ASSERT_EQUAL(USBIP_SET_REBOOT, usbip_core_check_enable(false, true, true, true));
}

/* ---- usbip_core_boot_state ---- */

TEST_CASE("Boot decision: disabled: off", "[usbip]")
{
    TEST_ASSERT_EQUAL(USBIP_STATE_OFF, usbip_core_boot_state(false, false));
    TEST_ASSERT_EQUAL(USBIP_STATE_OFF, usbip_core_boot_state(false, true));
}

TEST_CASE("Boot decision: enabled with a held USB stack: blocked", "[usbip]")
{
    TEST_ASSERT_EQUAL(USBIP_STATE_BLOCKED, usbip_core_boot_state(true, true));
}

TEST_CASE("Boot decision: enabled otherwise: no device", "[usbip]")
{
    TEST_ASSERT_EQUAL(USBIP_STATE_NO_DEVICE, usbip_core_boot_state(true, false));
}

/* ---- usbip_state_name ---- */

TEST_CASE("On each state: names it as the API shows it", "[usbip]")
{
    TEST_ASSERT_EQUAL_STRING("off", usbip_state_name(USBIP_STATE_OFF));
    TEST_ASSERT_EQUAL_STRING("blocked", usbip_state_name(USBIP_STATE_BLOCKED));
    TEST_ASSERT_EQUAL_STRING("no_device", usbip_state_name(USBIP_STATE_NO_DEVICE));
    TEST_ASSERT_EQUAL_STRING("unsupported", usbip_state_name(USBIP_STATE_UNSUPPORTED));
    TEST_ASSERT_EQUAL_STRING("exportable", usbip_state_name(USBIP_STATE_EXPORTABLE));
    TEST_ASSERT_EQUAL_STRING("attached", usbip_state_name(USBIP_STATE_ATTACHED));
}
