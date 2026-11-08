// USB transport of the mux (from usb_serial's usb_host_glue.c): the USB host and its usb_lib task, the CDC driver, the
// one open device. Only the owner task opens or closes the device and installs anything; the callbacks post messages
// or hand received bytes to the port.
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board_cores.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_private/cdc_host_common.h" // cdc_dev_t: the USB device a VCP open picked
#include "radio_mux_priv.h"
#include "usb/cdc_acm_host.h"
#include "usb/usb_host.h"
#include "usb/usb_types_ch9.h"
#include "usb/vcp_ch34x.h"
#include "usb/vcp_cp210x.h"

static const char *TAG = "radio_mux";

#define LIB_TASK_PRIORITY 7
#define LIB_TASK_STACK 4096
#define CDC_TASK_PRIORITY 7
#define CDC_TASK_STACK 4096
#define LIB_POLL_MS 50
#define OPEN_TIMEOUT_MS 1000
#define USB_BUF_SIZE 512 // the OUT buffer
// The bulk IN transfer is one max packet (0 = the endpoint's MPS, the driver's default): a transfer completes only on
// a short packet or when full, and the bridges send no ZLP, so a 512-byte transfer held a burst ending on a packet
// boundary until the radio sent again (~2 s late answers on a CH340, hardware 2026-11-07). The driver keeps eight such
// transfers in flight (PATCHES.md patch 32; Linux's usb-serial keeps two, but completes them in interrupt context):
// while some complete into data_cb and are resubmitted, the bridge completes into the others (with one or two, the
// CH340 overran its UART FIFO at 460800). PATCHES.md patch 31 keeps that length after the buffer's cache-line rounding.
#define USB_IN_BUF_SIZE 0
#define MAX_INTERFACES 16
#define MAX_ADDRS 8
#define DATA_INTERFACE 0 // the interface the VCP drivers open (CP2102N, CH340: the only one)
#define CP210X_REQ_OUT 0x41 // vendor, interface, host to device
#define CP210X_SET_FLOW 0x13
#define ASCII_FIRST 0x20
#define ASCII_LAST 0x7E
#define UART_ERR_LOG_US (10 * 1000 * 1000) // at most one UART error warning per 10 s per port
#define UART_ERR_LOG_BYTES 8               // raw bytes shown in it

static bool s_host_up;          // owner only
static cdc_acm_dev_hdl_t s_hdl; // written by the owner only (under the port's tx_lock), read by senders under it
static radio_mux_driver_t s_driver;
static uint32_t s_baud; // the line speed set on the open device (sizes the TX timeout)
// Owner only: a device whose cdc_acm_host_close failed (the driver keeps it listed, half closed). Its handle is kept
// and closed again (usb_transport_close_retry); while it is, nothing else is opened.
static cdc_acm_dev_hdl_t s_close_hdl;
static radio_mux_close_retry_t s_close_retry;

/* ---- callbacks (USB host / CDC driver task context) ---- */

// UTF-16LE string descriptor to printable ASCII; other characters are dropped.
static void str_ascii(const usb_str_desc_t *d, char *out, size_t cap)
{
    size_t n = 0;

    if (d != NULL && d->bLength > USB_STR_DESC_SIZE) {
        const size_t chars = (d->bLength - USB_STR_DESC_SIZE) / 2;

        for (size_t i = 0; i < chars && n + 1 < cap; i++) {
            const uint16_t c = d->wData[i];

            if (c >= ASCII_FIRST && c <= ASCII_LAST) {
                out[n++] = (char)c;
            }
        }
    }
    out[n] = '\0';
}

static void new_dev_cb(usb_device_handle_t dev)
{
    const usb_device_desc_t *dd;
    const usb_config_desc_t *cd;
    usb_device_info_t info;
    uint8_t classes[MAX_INTERFACES];
    size_t n = 0;

    if (usb_host_get_device_descriptor(dev, &dd) != ESP_OK || usb_host_device_info(dev, &info) != ESP_OK) {
        return;
    }
    if (usb_host_get_active_config_descriptor(dev, &cd) == ESP_OK) {
        const uint8_t *p = (const uint8_t *)cd;
        const size_t total = cd->wTotalLength;

        for (size_t off = 0; off + 2 <= total && n < MAX_INTERFACES;) {
            const uint8_t len = p[off];

            if (len < 2) {
                break;
            }
            if (p[off + 1] == USB_B_DESCRIPTOR_TYPE_INTERFACE && off + 6 <= total) {
                classes[n++] = p[off + 5]; // bInterfaceClass
            }
            off += len;
        }
    }
    mux_msg_t m = {
        .type = MUX_MSG_NEW_DEV,
        .dev = {
            .vid = dd->idVendor,
            .pid = dd->idProduct,
            .addr = info.dev_addr,
            .driver = radio_mux_pick_driver(dd->idVendor, dd->idProduct, dd->bDeviceClass, classes, n),
        },
    };

    str_ascii(info.str_desc_serial_num, m.dev.serial, sizeof(m.dev.serial));
    str_ascii(info.str_desc_product, m.dev.product, sizeof(m.dev.product));
    if (!mux_post(&m)) {
        ESP_LOGW(TAG, "new device %04x:%04x: queue full, ignored", dd->idVendor, dd->idProduct);
    }
}

// Must consume everything and return true: on the P4 the driver cannot append to its IN buffer. `data` is valid only
// during the call; mux_rx_bytes copies what it keeps.
static bool data_cb(const uint8_t *data, size_t len, void *arg)
{
    mux_port_t *p = arg;

    xSemaphoreTake(p->rx_lock, portMAX_DELAY);
    mux_rx_bytes(p, data, len);
    xSemaphoreGive(p->rx_lock);
    return true;
}

// A CH34x status packet (patch 30 delivers it as CDC_ACM_HOST_VENDOR_NOTIFICATION): a UART error bit counts one
// uart_errors; the warning, with the raw bytes, at most once per UART_ERR_LOG_US. Nothing is logged per packet.
static void ch34x_status(mux_port_t *p, const uint8_t *pkt, size_t len)
{
    if (!radio_mux_ch34x_uart_error(pkt, len)) {
        return;
    }
    taskENTER_CRITICAL(&p->lock);
    const uint32_t total = ++p->ctr.uart_errors;
    taskEXIT_CRITICAL(&p->lock);
    const int64_t now = esp_timer_get_time();

    if (p->uart_err_log_us != 0 && now - p->uart_err_log_us < UART_ERR_LOG_US) {
        return;
    }
    p->uart_err_log_us = now;
    char hex[UART_ERR_LOG_BYTES * 3 + 1] = "";
    const size_t n = len < UART_ERR_LOG_BYTES ? len : UART_ERR_LOG_BYTES;

    for (size_t i = 0; i < n; i++) {
        snprintf(hex + i * 3, sizeof(hex) - i * 3, "%02x%s", pkt[i], i + 1 < n ? " " : "");
    }
    ESP_LOGW(TAG, "%04x:%04x: the CH34x reports a UART error (%s), %lu so far", p->dev.vid, p->dev.pid, hex,
             (unsigned long)total);
}

static void event_cb(const cdc_acm_host_dev_event_data_t *event, void *ctx)
{
    mux_port_t *p = ctx;

    switch (event->type) {
    case CDC_ACM_HOST_DEVICE_DISCONNECTED: {
        const mux_msg_t m = {.type = MUX_MSG_GONE, .hdl = event->data.cdc_hdl};

        atomic_store(&p->tx_ok, false); // senders fail at once from here on, before the owner gets to the message
        atomic_store(&p->dev_gone, true); // the owner's presence check closes the device if the message is lost
        if (!mux_post(&m)) {
            ESP_LOGE(TAG, "unplug event lost: queue full");
        }
        break;
    }
    case CDC_ACM_HOST_ERROR: {
        // A bulk IN or notification transfer failed: the driver does not resubmit it, so the device's polling has
        // stopped. The owner reopens the device (radio_mux's USB error check); one message per flagged error.
        const mux_msg_t m = {.type = MUX_MSG_USB_ERROR};

        ESP_LOGW(TAG, "CDC error %d", event->data.error);
        if (!atomic_exchange(&p->usb_error, true) && !mux_post(&m)) {
            ESP_LOGW(TAG, "USB error message lost: queue full (the owner's poll finds the flag)");
        }
        break;
    }
    case CDC_ACM_HOST_VENDOR_NOTIFICATION: // other drivers: ignored
        if (p->dev.driver == RADIO_MUX_DRV_CH34X) {
            ch34x_status(p, event->data.vendor_notification.data, event->data.vendor_notification.len);
        }
        break;
    default:
        break;
    }
}

/* ---- usb_lib task ---- */

typedef struct {
    SemaphoreHandle_t done;
    esp_err_t err;
} lib_start_t;

// Installs the USB host from NET_CORE, so its interrupt is allocated there (esp_intr_alloc takes the calling core),
// then handles the host's events for the life of the firmware (the mux never stops it). The USB interrupt starts each
// queued bulk IN transfer: on MGMT_CORE, management code that masks interrupts (the chip temperature read changing its
// range busy-waits ~600 us in a spinlock) held it off long enough for a CH340 at 460800 to overrun (hardware
// 2026-11-07). NET_CORE carries the network path and the C6's transport already.
static void lib_task(void *arg)
{
    lib_start_t *start = arg;
    const usb_host_config_t host_cfg = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LOWMED,
        // peripheral_map 0: the default, the High-Speed OTG peripheral (the console sits on the full-speed PHY)
    };
    uint32_t flags;

    start->err = usb_host_install(&host_cfg);
    const bool up = start->err == ESP_OK;

    xSemaphoreGive(start->done); // `start` is the caller's: not touched after this
    if (!up) {
        vTaskDelete(NULL);
    }
    for (;;) {
        flags = 0;
        usb_host_lib_handle_events(pdMS_TO_TICKS(LIB_POLL_MS), &flags);
    }
}

esp_err_t usb_transport_start(void)
{
    if (s_host_up) {
        return ESP_OK;
    }
    lib_start_t start = {.done = xSemaphoreCreateBinary(), .err = ESP_FAIL};

    ESP_RETURN_ON_FALSE(start.done, ESP_ERR_NO_MEM, TAG, "usb_lib start semaphore");
    if (xTaskCreatePinnedToCore(lib_task, "usb_lib", LIB_TASK_STACK, &start, LIB_TASK_PRIORITY, NULL, NET_CORE) !=
            pdPASS) {
        vSemaphoreDelete(start.done);
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(start.done, portMAX_DELAY);
    vSemaphoreDelete(start.done);
    ESP_RETURN_ON_ERROR(start.err, TAG, "usb_host_install");
    const cdc_acm_host_driver_config_t cdc_cfg = {
        .driver_task_stack_size = CDC_TASK_STACK,
        .driver_task_priority = CDC_TASK_PRIORITY,
        .xCoreID = NET_CORE,
        .new_dev_cb = new_dev_cb,
    };

    // The usb_lib task stays: uninstalling the host under it is the shutdown dance usb_serial needed, and the mux
    // runs the host for good. Without the CDC driver no device ever opens.
    ESP_RETURN_ON_ERROR(cdc_acm_host_install(&cdc_cfg), TAG, "cdc_acm_host_install");
    s_host_up = true;
    ESP_LOGI(TAG, "USB host started");
    return ESP_OK;
}

/* ---- the device ---- */

// Address of the USB device under a CDC handle; 0 (never a device address) when unknown.
static uint8_t opened_addr(cdc_acm_dev_hdl_t hdl)
{
    const cdc_dev_t *cdc = (const cdc_dev_t *)hdl;
    usb_device_info_t info;

    if (!cdc || !cdc->dev_hdl || usb_host_device_info(cdc->dev_hdl, &info) != ESP_OK) {
        return 0;
    }
    return info.dev_addr;
}

static const char *driver_label(radio_mux_driver_t d)
{
    const char *name = radio_mux_driver_name(d);

    return name ? name : "none";
}

// Line coding (baud, 8N1) with read-back, then flow control. On an error it closes the device.
static esp_err_t serial_setup(mux_port_t *p, uint32_t baud, bool flow, char *err, size_t cap)
{
    cdc_acm_line_coding_t lc = {.dwDTERate = baud, .bCharFormat = 0, .bParityType = 0, .bDataBits = 8};
    esp_err_t ret = cdc_acm_host_line_coding_set(s_hdl, &lc);

    err[0] = '\0';
    if (ret != ESP_OK) {
        snprintf(err, cap, "baud %lu not accepted: %s", (unsigned long)baud, esp_err_to_name(ret));
        usb_transport_close(p);
        return ret;
    }
    cdc_acm_line_coding_t back = {0};

    if (cdc_acm_host_line_coding_get(s_hdl, &back) == ESP_OK) { // CH34x cannot read it back: nothing to compare
        const radio_mux_baud_t v = radio_mux_baud_verdict(baud, back.dwDTERate);

        if (v != RADIO_MUX_BAUD_EXACT) {
            snprintf(err, cap, "baud %lu read back as %lu", (unsigned long)baud, (unsigned long)back.dwDTERate);
            ESP_LOGW(TAG, "%s", err);
        }
        if (v == RADIO_MUX_BAUD_FAR) {
            usb_transport_close(p);
            return ESP_ERR_INVALID_RESPONSE;
        }
    }
    s_baud = baud;
    if (s_driver == RADIO_MUX_DRV_CP210X) {
        uint8_t payload[RADIO_MUX_CP210X_FLOW_LEN];

        radio_mux_cp210x_flow(flow, payload);
        ret = cdc_acm_host_send_custom_request(s_hdl, CP210X_REQ_OUT, CP210X_SET_FLOW, 0, DATA_INTERFACE,
                                               sizeof(payload), payload);
        if (ret != ESP_OK) {
            snprintf(err, cap, "flow control failed: %s", esp_err_to_name(ret));
        }
    } else if (flow) {
        snprintf(err, cap, "flow control not supported by %s", driver_label(s_driver));
    }
    return ESP_OK;
}

// The port's frame pool (DMA-capable, cache-line aligned, like the C6's), the decoder's discard buffer, the HDLC TX
// buffer and the byte ring, allocated on the first open and kept (a consumer may hold slots past its detach; a dongle
// that comes and goes does not churn the heap). About 39 KB that a board without a dongle never takes.
static esp_err_t alloc_bufs(mux_port_t *p)
{
    if (p->pool.slots) {
        return ESP_OK;
    }
    const size_t tx_cap = RADIO_MUX_HDLC_ENCODED_MAX(RADIO_MUX_FRAME_MAX);
    uint8_t *slots = heap_caps_aligned_alloc(MUX_CACHE_LINE, (size_t)MUX_SLOT_SIZE * RADIO_MUX_POOL_SLOTS,
                                             MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    uint8_t *discard = malloc(MUX_SLOT_SIZE);
    uint8_t *tx = malloc(tx_cap);
    RingbufHandle_t ring = xRingbufferCreate(MUX_RX_RING_SIZE, RINGBUF_TYPE_BYTEBUF);

    if (!slots || !discard || !tx || !ring) {
        heap_caps_free(slots);
        free(discard);
        free(tx);
        if (ring) {
            vRingbufferDelete(ring);
        }
        return ESP_ERR_NO_MEM;
    }
    p->discard = discard;
    p->tx_buf = tx;
    p->tx_cap = tx_cap;
    p->ring = ring;
    taskENTER_CRITICAL(&p->lock);
    radio_mux_pool_init(&p->pool, slots, MUX_SLOT_SIZE, RADIO_MUX_POOL_SLOTS);
    taskEXIT_CRITICAL(&p->lock);
    return ESP_OK;
}

esp_err_t usb_transport_open(mux_port_t *p, const mux_usb_dev_t *dev, uint32_t baud, bool flow, char *err, size_t cap)
{
    err[0] = '\0';
    if (!s_host_up || s_hdl) {
        snprintf(err, cap, "%s", s_host_up ? "a device is already open" : "USB host not running");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_close_hdl && s_close_retry.gave_up && cdc_acm_host_close(s_close_hdl) == ESP_OK) {
        ESP_LOGI(TAG, "the device whose close failed is closed now");
        s_close_hdl = NULL;
        s_close_retry.gave_up = false;
    }
    if (s_close_hdl) {
        snprintf(err, cap, "%s", s_close_retry.gave_up ? RADIO_MUX_USB_CLOSE_FAILED_REASON :
                 RADIO_MUX_USB_CLOSE_PENDING_REASON);
        return ESP_ERR_INVALID_STATE;
    }
    if (alloc_bufs(p) != ESP_OK) {
        snprintf(err, cap, "out of memory for the USB radio buffers");
        return ESP_ERR_NO_MEM;
    }
    const cdc_acm_host_device_config_t cfg = {
        .connection_timeout_ms = OPEN_TIMEOUT_MS,
        .out_buffer_size = USB_BUF_SIZE,
        .in_buffer_size = USB_IN_BUF_SIZE,
        .event_cb = event_cb,
        .data_cb = data_cb,
        .user_arg = p,
    };
    cdc_acm_dev_hdl_t hdl = NULL;
    esp_err_t ret;

    atomic_store(&p->dev_gone, false);
    atomic_store(&p->usb_error, false); // an error of an earlier device is not this one's
    switch (dev->driver) {
    case RADIO_MUX_DRV_CP210X:
        ret = cp210x_vcp_open(dev->pid, DATA_INTERFACE, &cfg, &hdl);
        break;
    case RADIO_MUX_DRV_CH34X:
        ret = ch34x_vcp_open(dev->pid, DATA_INTERFACE, &cfg, &hdl);
        break;
    case RADIO_MUX_DRV_CDC_ACM: {
        const cdc_acm_host_open_config_t open_cfg = {
            .vid = dev->vid,
            .pid = dev->pid,
            .interface_idx = 0,
            .dev_addr = dev->addr,
            .connection_timeout_ms = OPEN_TIMEOUT_MS,
            .out_buffer_size = USB_BUF_SIZE,
            .in_buffer_size = USB_IN_BUF_SIZE,
            .event_cb = event_cb,
            .data_cb = data_cb,
            .user_arg = p,
        };

        ret = cdc_acm_host_open_v2(&open_cfg, &hdl);
        break;
    }
    default:
        snprintf(err, cap, "no driver");
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (ret != ESP_OK) {
        snprintf(err, cap, "open failed: %s", esp_err_to_name(ret));
        ESP_LOGW(TAG, "%04x:%04x %s", dev->vid, dev->pid, err);
        return ret;
    }
    // The VCP drivers open the first device with the VID/PID (any address): with two identical bridges that can be
    // the other one. Only the detected device may be opened.
    const uint8_t addr = opened_addr(hdl);

    if (addr != dev->addr) {
        snprintf(err, cap, "an identical device at address %u answered the open (one at a time)", addr);
        ESP_LOGW(TAG, "%04x:%04x at %u: %s", dev->vid, dev->pid, dev->addr, err);
        if (cdc_acm_host_close(hdl) != ESP_OK) {
            s_close_hdl = hdl; // closed again by usb_transport_close_retry
            radio_mux_close_retry_start(&s_close_retry, esp_timer_get_time() / 1000);
        }
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(p->tx_lock, portMAX_DELAY);
    s_hdl = hdl;
    s_driver = dev->driver;
    xSemaphoreGive(p->tx_lock);
    ESP_RETURN_ON_ERROR(serial_setup(p, baud, flow, err, cap), TAG, "%04x:%04x: %s", dev->vid, dev->pid, err);
    // DTR and RTS together, both asserted, in one request (one SET_MHS on CP210x): the auto-reset circuit never sees
    // the EN/IO0 pulse pattern. Never toggled afterwards.
    ret = cdc_acm_host_set_control_line_state(s_hdl, true, true);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "DTR/RTS: %s", esp_err_to_name(ret));
    }
    ESP_LOGI(TAG, "%04x:%04x open (%s, %lu baud)", dev->vid, dev->pid, driver_label(dev->driver),
             (unsigned long)baud);
    return ESP_OK;
}

esp_err_t usb_transport_line(mux_port_t *p, uint32_t baud, bool flow, char *err, size_t cap)
{
    err[0] = '\0';
    if (!s_hdl) {
        snprintf(err, cap, "not open");
        return ESP_ERR_INVALID_STATE;
    }
    return serial_setup(p, baud, flow, err, cap);
}

void usb_transport_close(mux_port_t *p)
{
    if (!s_hdl) {
        return;
    }
    xSemaphoreTake(p->tx_lock, portMAX_DELAY); // a sender inside the driver leaves within its TX timeout
    const cdc_acm_dev_hdl_t hdl = s_hdl;

    s_hdl = NULL;
    s_driver = RADIO_MUX_DRV_NONE;
    s_baud = 0;
    xSemaphoreGive(p->tx_lock);
    const esp_err_t err = cdc_acm_host_close(hdl);

    atomic_store(&p->usb_error, false); // nothing is open to reopen
    if (err != ESP_OK) {
        // The driver kept the device (a transfer still in flight): the handle is kept and closed again in the
        // background; the port opens nothing until then.
        ESP_LOGW(TAG, "cdc_acm_host_close: %s: closing again in the background", esp_err_to_name(err));
        s_close_hdl = hdl;
        radio_mux_close_retry_start(&s_close_retry, esp_timer_get_time() / 1000);
    }
}

bool usb_transport_close_pending(void)
{
    return s_close_hdl != NULL && s_close_retry.pending;
}

radio_mux_close_act_t usb_transport_close_retry(void)
{
    const int64_t now = esp_timer_get_time() / 1000;
    const radio_mux_close_act_t due = s_close_hdl ? radio_mux_close_retry_due(&s_close_retry, now) :
                                      RADIO_MUX_CLOSE_IDLE;

    if (due != RADIO_MUX_CLOSE_TRY) {
        return due;
    }
    const esp_err_t err = cdc_acm_host_close(s_close_hdl);
    const radio_mux_close_act_t act = radio_mux_close_retry_result(&s_close_retry, esp_timer_get_time() / 1000,
                                                                   err == ESP_OK);

    if (act == RADIO_MUX_CLOSE_DONE) {
        s_close_hdl = NULL;
        ESP_LOGI(TAG, "the device whose close failed is closed now");
    } else if (act == RADIO_MUX_CLOSE_GAVE_UP) {
        ESP_LOGE(TAG, "closing the USB device failed %d times (%s): given up, the port opens nothing until it closes",
                 RADIO_MUX_CLOSE_RETRIES, esp_err_to_name(err));
    }
    return act;
}

bool usb_transport_is_open(void)
{
    return s_hdl != NULL;
}

bool usb_transport_is(const void *hdl)
{
    return hdl && hdl == (const void *)s_hdl;
}

esp_err_t usb_transport_write(const uint8_t *data, size_t n)
{
    if (!s_hdl) {
        return ESP_ERR_INVALID_STATE;
    }
    return cdc_acm_host_data_tx_blocking(s_hdl, data, n, radio_mux_tx_timeout_ms(n, s_baud));
}

bool usb_transport_dev_attached(uint8_t addr)
{
    uint8_t list[MAX_ADDRS];
    int n = 0;

    if (usb_host_device_addr_list_fill(sizeof(list), list, &n) != ESP_OK) {
        return true; // unknown: keep what the list shows
    }
    for (int i = 0; i < n; i++) {
        if (list[i] == addr) {
            return true;
        }
    }
    return false;
}

#ifdef OTBR_TEST_BUILD
esp_err_t radio_mux_test_usb_ctrl(uint8_t bm_request_type, uint8_t b_request, uint16_t w_value, uint16_t w_index,
                                  uint16_t w_length, uint8_t *data)
{
    const cdc_acm_dev_hdl_t hdl = s_hdl;

    if (!hdl) {
        return ESP_ERR_INVALID_STATE;
    }
    return cdc_acm_host_send_custom_request(hdl, bm_request_type, b_request, w_value, w_index, w_length, data);
}
#endif
