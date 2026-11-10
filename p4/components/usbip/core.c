#include "usbip_core.h"

#include <stdio.h>
#include <string.h>

#define DESC_DEVICE 1
#define DESC_CONFIG 2
#define DESC_INTERFACE 4
#define DESC_ENDPOINT 5
#define CLASS_HUB 9

#define REQ_CLEAR_FEATURE 1
#define REQ_SET_FEATURE 3
#define REQ_SET_ADDRESS 5
#define REQ_SET_CONFIGURATION 9
#define REQ_SET_INTERFACE 11
#define FEATURE_ENDPOINT_HALT 0
#define FEATURE_PORT_RESET 4

// bmRequestType: standard request to the device, interface, endpoint; SET_FEATURE to a port is class | other.
#define RT_STD_DEVICE_OUT 0x00
#define RT_STD_INTERFACE_OUT 0x01
#define RT_STD_ENDPOINT_OUT 0x02
#define RT_CLASS_OTHER_OUT 0x23

// Checks that the descriptors tile the configuration exactly up to wTotalLength. Returns the total length, 0 when the
// configuration is truncated or malformed.
static size_t cfg_total(const uint8_t *cfg, size_t cfg_len)
{
    if (cfg == NULL || cfg_len < 9 || cfg[0] != 9 || cfg[1] != DESC_CONFIG) {
        return 0;
    }
    size_t total = (size_t)cfg[2] | ((size_t)cfg[3] << 8);

    if (total < 9 || total > cfg_len) {
        return 0;
    }
    for (size_t o = 0; o < total;) {
        if (total - o < 2 || cfg[o] < 2 || cfg[o] > total - o) {
            return 0;
        }
        o += cfg[o];
    }
    return total;
}

static bool is_ep_desc(const uint8_t *d)
{
    return d[1] == DESC_ENDPOINT && d[0] >= 7;
}

const char *usbip_state_name(usbip_state_t s)
{
    switch (s) {
    case USBIP_STATE_OFF:
        return "off";
    case USBIP_STATE_BLOCKED:
        return "blocked";
    case USBIP_STATE_NO_DEVICE:
        return "no_device";
    case USBIP_STATE_UNSUPPORTED:
        return "unsupported";
    case USBIP_STATE_EXPORTABLE:
        return "exportable";
    case USBIP_STATE_ATTACHED:
        return "attached";
    }
    return "unknown";
}

esp_err_t usbip_core_dev_record(const uint8_t dev_desc[18], const uint8_t *cfg, size_t cfg_len, uint8_t addr,
                                uint32_t speed, usbip_dev_rec_t *out, char *why, size_t cap)
{
    if (dev_desc == NULL || out == NULL || dev_desc[0] != 18 || dev_desc[1] != DESC_DEVICE) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t total = cfg_total(cfg, cfg_len);

    if (total == 0) {
        return ESP_ERR_INVALID_SIZE;
    }

    usbip_dev_rec_t r = {0};
    unsigned eps = 0;
    bool hub = dev_desc[4] == CLASS_HUB;
    bool alt0 = false;

    for (size_t o = 0; o < total; o += cfg[o]) {
        if (cfg[o + 1] == DESC_INTERFACE && cfg[o] >= 9) {
            alt0 = cfg[o + 3] == 0;
            if (alt0) {
                hub = hub || cfg[o + 5] == CLASS_HUB;
                if (r.num_ifaces < USBIP_IFACES_MAX) {
                    r.ifaces[r.num_ifaces] = (usbip_iface_rec_t) {
                        .cls = cfg[o + 5], .subcls = cfg[o + 6], .proto = cfg[o + 7]
                    };
                    r.num_ifaces++;
                }
            }
        } else if (is_ep_desc(&cfg[o]) && alt0) {
            eps++;
        }
    }
    if (hub) {
        if (why != NULL && cap > 0) {
            snprintf(why, cap, "hubs are not supported");
        }
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (eps > USBIP_EP_MAX) {
        if (why != NULL && cap > 0) {
            snprintf(why, cap, "%u endpoints in the active configuration, at most %d are supported", eps,
                     USBIP_EP_MAX);
        }
        return ESP_ERR_NOT_SUPPORTED;
    }

    snprintf(r.path, sizeof(r.path), "%s", USBIP_PATH);
    snprintf(r.busid, sizeof(r.busid), "%s", USBIP_BUSID);
    r.busnum = 1;
    r.devnum = addr;
    r.speed = speed;
    r.vid = (uint16_t)(dev_desc[8] | (dev_desc[9] << 8));
    r.pid = (uint16_t)(dev_desc[10] | (dev_desc[11] << 8));
    r.bcd_device = (uint16_t)(dev_desc[12] | (dev_desc[13] << 8));
    r.dev_class = dev_desc[4];
    r.dev_subclass = dev_desc[5];
    r.dev_proto = dev_desc[6];
    r.config_value = cfg[5];
    r.num_configs = dev_desc[17];
    *out = r;
    return ESP_OK;
}

size_t usbip_core_endpoints(const uint8_t *cfg, size_t cfg_len, uint8_t iface, uint8_t alt, usbip_ep_info_t *out,
                            size_t cap)
{
    size_t total = cfg_total(cfg, cfg_len);
    size_t n = 0;
    bool match = false;

    for (size_t o = 0; o < total; o += cfg[o]) {
        if (cfg[o + 1] == DESC_INTERFACE && cfg[o] >= 9) {
            match = cfg[o + 2] == iface && cfg[o + 3] == alt;
        } else if (is_ep_desc(&cfg[o]) && match) {
            if (out != NULL && n < cap) {
                out[n] = (usbip_ep_info_t) {
                    .addr = cfg[o + 2],
                    .type = cfg[o + 3] & 0x3,
                    .mps = (uint16_t)((cfg[o + 4] | (cfg[o + 5] << 8)) & 0x7ff),
                };
            }
            n++;
        }
    }
    return n;
}

usbip_req_t usbip_core_classify_setup(const uint8_t setup[8], uint8_t active_config)
{
    uint8_t type = setup[0];
    uint8_t req = setup[1];
    uint16_t value = (uint16_t)(setup[2] | (setup[3] << 8));

    if (type == RT_STD_DEVICE_OUT && req == REQ_SET_CONFIGURATION) {
        return value == active_config ? USBIP_REQ_SET_CONFIG_SAME : USBIP_REQ_SET_CONFIG_OTHER;
    }
    if (type == RT_STD_DEVICE_OUT && req == REQ_SET_ADDRESS) {
        return USBIP_REQ_SET_ADDRESS;
    }
    if (type == RT_STD_INTERFACE_OUT && req == REQ_SET_INTERFACE) {
        return USBIP_REQ_SET_INTERFACE;
    }
    if (type == RT_STD_ENDPOINT_OUT && req == REQ_CLEAR_FEATURE && value == FEATURE_ENDPOINT_HALT) {
        return USBIP_REQ_CLEAR_HALT;
    }
    if (type == RT_CLASS_OTHER_OUT && req == REQ_SET_FEATURE && value == FEATURE_PORT_RESET) {
        return USBIP_REQ_PORT_RESET;
    }
    return USBIP_REQ_FORWARD;
}

int32_t usbip_core_status_errno(usbip_xfer_status_t st, bool short_in, uint32_t flags)
{
    switch (st) {
    case USBIP_XS_COMPLETED:
        return short_in && (flags & USBIP_URB_SHORT_NOT_OK) ? -USBIP_EREMOTEIO : 0;
    case USBIP_XS_STALL:
        return -USBIP_EPIPE;
    case USBIP_XS_OVERFLOW:
        return -USBIP_EOVERFLOW;
    case USBIP_XS_ERROR:
        return -USBIP_EPROTO;
    case USBIP_XS_NO_DEVICE:
        return -USBIP_ENODEV;
    case USBIP_XS_CANCELED:
        return -USBIP_ECONNRESET;
    }
    return -USBIP_EPROTO;
}

int32_t usbip_core_check_submit(const usbip_cmd_t *c, uint32_t devid, const usbip_ep_info_t *ep)
{
    if (c->devid != devid) {
        return -USBIP_ENODEV;
    }
    if (ep == NULL || ep->type == 1) {
        return -USBIP_EINVAL;
    }
    int32_t len = c->submit.length;

    if (len < 0 || len > USBIP_XFER_MAX || (ep->type == 0 && len > USBIP_CTRL_DATA_MAX)) {
        return -USBIP_EINVAL;
    }
    return 0;
}

bool usbip_core_alloc_fits(size_t free, size_t n, size_t floor)
{
    return n <= free && free - n >= floor;
}

usbip_set_verdict_t usbip_core_check_enable(bool want, bool stored, bool long_op, bool usb_stack_held)
{
    if (want == stored) {
        return USBIP_SET_NOOP;
    }
    if (want && usb_stack_held) {
        return USBIP_SET_REFUSE_IN_USE;
    }
    if (want && long_op) {
        return USBIP_SET_REFUSE_BUSY;
    }
    return USBIP_SET_REBOOT;
}

usbip_state_t usbip_core_boot_state(bool enabled, bool usb_stack_held)
{
    if (!enabled) {
        return USBIP_STATE_OFF;
    }
    return usb_stack_held ? USBIP_STATE_BLOCKED : USBIP_STATE_NO_DEVICE;
}
