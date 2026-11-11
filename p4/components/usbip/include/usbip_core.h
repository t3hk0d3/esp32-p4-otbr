// USB/IP descriptor parsing and decision tables (spec sections 5, 6.3, 6.4). Pure: raw descriptor bytes in, verdicts
// out, no esp-usb types and no I/O, so it builds and is tested on the linux target.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "usbip_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

#define USBIP_BUSID "1-1"
#define USBIP_PATH "/sys/devices/platform/esp32p4-otg/usb1/1-1"
#define USBIP_FIRST_BYTE_MS 10000
#define USBIP_KEEPALIVE_IDLE_S 10
#define USBIP_KEEPALIVE_INTERVAL_S 5
#define USBIP_KEEPALIVE_PROBES 3
#define USBIP_CTRL_DATA_MAX 4096
// One transfer: Linux usb-storage reads up to 120 KB (max_sectors 240). Transfer buffers live in PSRAM
// (CONFIG_USB_HOST_DWC_DMA_CAP_MEMORY_IN_PSRAM, spec 8.1).
#define USBIP_XFER_MAX (120 * 1024)
#define USBIP_EP_MAX 15
#define USBIP_INFLIGHT_BYTES (256 * 1024)
#define USBIP_INFLIGHT_XFERS 32
#define USBIP_RESERVE_SMALL 2048
// Shared IN bytes, one large transfer and every endpoint's small reserve: one large transfer always fits once no other
// is in flight (408 KB).
#define USBIP_INFLIGHT_HARD (USBIP_INFLIGHT_BYTES + USBIP_XFER_MAX + (USBIP_EP_MAX + 1) * USBIP_RESERVE_SMALL)
#define USBIP_WAIT_BYTES 1048576
// esp-usb's per-transfer limit is 131071 bytes floored to whole packets: 130048 for a 1024-byte endpoint, the worst.
_Static_assert(USBIP_XFER_MAX <= 130048, "USBIP_XFER_MAX must fit esp-usb's per-transfer limit on every endpoint");
_Static_assert(USBIP_WAIT_BYTES >= USBIP_XFER_MAX, "a waiting transfer of USBIP_XFER_MAX must fit the waiting cap");
#define USBIP_EP0_UNLINK_MS 2000

#define USBIP_ST_OK 0
#define USBIP_ST_DEV_BUSY 2
#define USBIP_ST_NODEV 4

// Linux errno values. The client is Linux, so these are fixed here and not taken from the host libc, whose numbers
// differ for some of them.
#define USBIP_ENOMEM 12
#define USBIP_ENODEV 19
#define USBIP_EINVAL 22
#define USBIP_EPIPE 32
#define USBIP_EPROTO 71
#define USBIP_EOVERFLOW 75
#define USBIP_ECONNRESET 104
#define USBIP_EREMOTEIO 121

typedef enum {
    USBIP_STATE_OFF,
    USBIP_STATE_BLOCKED,
    USBIP_STATE_NO_DEVICE,
    USBIP_STATE_UNSUPPORTED,
    USBIP_STATE_EXPORTABLE,
    USBIP_STATE_ATTACHED,
} usbip_state_t;

typedef struct {
    uint8_t addr;
    uint8_t type; // USB transfer type: 0 control, 1 isochronous, 2 bulk, 3 interrupt
    uint16_t mps;
} usbip_ep_info_t;

typedef enum {
    USBIP_REQ_FORWARD,
    USBIP_REQ_SET_CONFIG_SAME,
    USBIP_REQ_SET_CONFIG_OTHER,
    USBIP_REQ_SET_INTERFACE,
    USBIP_REQ_CLEAR_HALT,
    USBIP_REQ_SET_ADDRESS,
    USBIP_REQ_PORT_RESET,
} usbip_req_t;

typedef enum {
    USBIP_XS_COMPLETED,
    USBIP_XS_STALL,
    USBIP_XS_OVERFLOW,
    USBIP_XS_ERROR,
    USBIP_XS_NO_DEVICE,
    USBIP_XS_CANCELED,
} usbip_xfer_status_t;

typedef enum {
    USBIP_SET_NOOP,
    USBIP_SET_REBOOT,
    USBIP_SET_REFUSE_IN_USE,
    USBIP_SET_REFUSE_BUSY,
    USBIP_SET_STORE_FAILED, // the flag could not be stored (NVS): nothing changed
} usbip_set_verdict_t;

// The state's name as the API shows it: off, blocked, no_device, unsupported, exportable, attached.
const char *usbip_state_name(usbip_state_t s);

// Builds the device record from the raw device (18 bytes) and active configuration descriptors. ESP_ERR_INVALID_SIZE
// on a truncated or malformed configuration, ESP_ERR_NOT_SUPPORTED (with a reason in why, when why is not NULL) for a
// hub or for more than USBIP_EP_MAX endpoints over the interfaces' alternate setting 0.
esp_err_t usbip_core_dev_record(const uint8_t dev_desc[18], const uint8_t *cfg, size_t cfg_len, uint8_t addr,
                                uint32_t speed, usbip_dev_rec_t *out, char *why, size_t cap);

// Lists the endpoints of one interface's alternate setting (at most cap entries stored); returns how many there are.
// 0 for an unknown interface or alt, and for a malformed configuration.
size_t usbip_core_endpoints(const uint8_t *cfg, size_t cfg_len, uint8_t iface, uint8_t alt, usbip_ep_info_t *out,
                            size_t cap);

usbip_req_t usbip_core_classify_setup(const uint8_t setup[8], uint8_t active_config);

// 0, or the negative Linux errno for the URB status.
int32_t usbip_core_status_errno(usbip_xfer_status_t st, bool short_in, uint32_t flags);

// 0 or a negative Linux errno. ep is the claimed endpoint (control: type 0), NULL when it is not claimed.
int32_t usbip_core_check_submit(const usbip_cmd_t *c, uint32_t devid, const usbip_ep_info_t *ep);

// Whether allocating n bytes out of `free` leaves at least `floor` free (false when n exceeds free).
bool usbip_core_alloc_fits(size_t free, size_t n, size_t floor);

usbip_set_verdict_t usbip_core_check_enable(bool want, bool stored, bool long_op, bool usb_stack_held);

usbip_state_t usbip_core_boot_state(bool enabled, bool usb_stack_held);

#ifdef __cplusplus
}
#endif
