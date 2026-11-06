// USB side of the proxy: the USB host and its usb_lib task, the CDC driver, the dongle and the dongle -> client ring.
// Only the owner task opens or closes the dongle and installs or uninstalls anything; the callbacks post messages.
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "board_cores.h"
#include "esp_check.h"
#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "usb/cdc_acm_host.h"
#include "usb/usb_host.h"
#include "usb/usb_types_ch9.h"
#include "usb/vcp_ch34x.h"
#include "usb/vcp_cp210x.h"
#include "usb_serial_priv.h"

static const char *TAG = "usb_serial";

#define LIB_TASK_PRIORITY 7
#define LIB_TASK_STACK 4096
#define CDC_TASK_PRIORITY 7
#define CDC_TASK_STACK 4096
#define LIB_POLL_MS 50
#define LIB_DONE_WAIT_MS 3000
#define UNINSTALL_TRIES 10
#define FREE_ALL_TRIES 50
#define RETRY_DELAY_MS 20
#define OPEN_TIMEOUT_MS 1000
#define USB_BUF_SIZE 512
#define MAX_INTERFACES 16
#define DATA_INTERFACE 0 // the interface the VCP drivers open (CP2102N, CH340: the only one)
#define RX_RING_SIZE (16 * 1024) // above USB_SERIAL_RING_MIN: a 100 ms dongle write at 921600 baud stays well inside
#define CP210X_REQ_OUT 0x41 // vendor, interface, host to device
#define CP210X_SET_FLOW 0x13

_Static_assert(RX_RING_SIZE >= USB_SERIAL_RING_MIN, "the dongle -> client ring must be above the TCP send buffer");

static bool s_host_up;     // owner only
static bool s_cdc_up;      // owner only
static atomic_bool s_lib_stop;
static atomic_bool s_all_free;
static SemaphoreHandle_t s_lib_done;
static esp_err_t s_lib_result;

static cdc_acm_dev_hdl_t s_hdl; // owner only
static usb_serial_driver_t s_driver;

static portMUX_TYPE s_rx_mux = portMUX_INITIALIZER_UNLOCKED;
static uint8_t s_rx_storage[RX_RING_SIZE];
static usb_serial_ring_t s_rx; // under s_rx_mux
static bool s_rx_accept;       // under s_rx_mux
static bool s_rx_overflow;     // under s_rx_mux: posted once, cleared by the owner's reset
static int64_t s_rx_first_us;  // under s_rx_mux

/* ---- callbacks (USB host / CDC driver task context) ---- */

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
    usb_serial_msg_t m = {
        .type = USB_SERIAL_MSG_NEW_DEV,
        .dev = {
            .vid = dd->idVendor,
            .pid = dd->idProduct,
            .addr = info.dev_addr,
            .driver = usb_serial_pick_driver(dd->idVendor, dd->idProduct, dd->bDeviceClass, classes, n),
        },
    };

    if (!usb_serial_post(&m)) {
        ESP_LOGW(TAG, "new device %04x:%04x: queue full, ignored", dd->idVendor, dd->idProduct);
    }
}

// Must consume everything and return true: on the P4 the driver cannot append to its IN buffer.
static bool data_cb(const uint8_t *data, size_t len, void *arg)
{
    bool post = false;

    taskENTER_CRITICAL(&s_rx_mux);
    if (s_rx_accept && !s_rx_overflow) {
        const bool was_empty = s_rx.len == 0;

        if (usb_serial_ring_push(&s_rx, data, len)) {
            if (was_empty) {
                s_rx_first_us = esp_timer_get_time();
            }
        } else {
            s_rx_overflow = true;
            post = true;
        }
    }
    taskEXIT_CRITICAL(&s_rx_mux);
    if (post) {
        const usb_serial_msg_t m = {.type = USB_SERIAL_MSG_OVERFLOW};

        usb_serial_post(&m);
    }
    return true;
}

static void event_cb(const cdc_acm_host_dev_event_data_t *event, void *ctx)
{
    switch (event->type) {
    case CDC_ACM_HOST_DEVICE_DISCONNECTED: {
        const usb_serial_msg_t m = {.type = USB_SERIAL_MSG_GONE, .hdl = event->data.cdc_hdl};

        if (!usb_serial_post(&m)) {
            ESP_LOGE(TAG, "unplug event lost: queue full");
        }
        break;
    }
    case CDC_ACM_HOST_ERROR:
        ESP_LOGW(TAG, "CDC error %d", event->data.error);
        break;
    default:
        break;
    }
}

/* ---- usb_lib task ---- */

static void lib_task(void *arg)
{
    uint32_t flags;

    for (;;) {
        flags = 0;
        usb_host_lib_handle_events(pdMS_TO_TICKS(LIB_POLL_MS), &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) {
            atomic_store(&s_all_free, true);
        }
        if (atomic_load(&s_lib_stop) && atomic_load(&s_all_free)) {
            break;
        }
    }
    // Out of usb_host_lib_handle_events for good: uninstall may run. Flags still pending make it answer
    // ESP_ERR_INVALID_STATE; one more pass consumes them.
    esp_err_t err = ESP_FAIL;

    for (int i = 0; i < UNINSTALL_TRIES; i++) {
        err = usb_host_uninstall();
        if (err != ESP_ERR_INVALID_STATE) {
            break;
        }
        usb_host_lib_handle_events(pdMS_TO_TICKS(RETRY_DELAY_MS), &flags);
    }
    s_lib_result = err;
    xSemaphoreGive(s_lib_done);
    vTaskDelete(NULL);
}

/* ---- host start / stop ---- */

esp_err_t usb_serial_host_start(void)
{
    if (s_host_up) {
        return ESP_OK;
    }
    if (!s_lib_done) {
        s_lib_done = xSemaphoreCreateBinary();
        ESP_RETURN_ON_FALSE(s_lib_done, ESP_ERR_NO_MEM, TAG, "semaphore");
    }
    const usb_host_config_t host_cfg = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LOWMED,
        // peripheral_map 0: the default, the High-Speed OTG peripheral (the console sits on the full-speed PHY)
    };

    ESP_RETURN_ON_ERROR(usb_host_install(&host_cfg), TAG, "usb_host_install");
    atomic_store(&s_lib_stop, false);
    atomic_store(&s_all_free, false);
    xSemaphoreTake(s_lib_done, 0);
    if (xTaskCreatePinnedToCore(lib_task, "usb_lib", LIB_TASK_STACK, NULL, LIB_TASK_PRIORITY, NULL, MGMT_CORE) !=
            pdPASS) {
        usb_host_uninstall();
        return ESP_ERR_NO_MEM;
    }
    s_host_up = true;
    const cdc_acm_host_driver_config_t cdc_cfg = {
        .driver_task_stack_size = CDC_TASK_STACK,
        .driver_task_priority = CDC_TASK_PRIORITY,
        .xCoreID = MGMT_CORE,
        .new_dev_cb = new_dev_cb,
    };
    const esp_err_t err = cdc_acm_host_install(&cdc_cfg);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cdc_acm_host_install: %s", esp_err_to_name(err));
        usb_serial_host_stop();
        return err;
    }
    s_cdc_up = true;
    ESP_LOGI(TAG, "USB host started");
    return ESP_OK;
}

void usb_serial_host_stop(void)
{
    if (!s_host_up) {
        return;
    }
    usb_serial_dongle_close();
    if (s_cdc_up) {
        esp_err_t err = ESP_FAIL;

        for (int i = 0; i < UNINSTALL_TRIES && err != ESP_OK; i++) {
            err = cdc_acm_host_uninstall();
            if (err != ESP_OK) {
                vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY_MS));
            }
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "cdc_acm_host_uninstall: %s", esp_err_to_name(err));
        }
        s_cdc_up = false;
    }
    atomic_store(&s_all_free, false);
    atomic_store(&s_lib_stop, true);
    // ESP_ERR_INVALID_STATE: the CDC client is still deregistering (its task does it); ESP_ERR_NOT_FINISHED: devices
    // are being freed, the all-free flag follows; ESP_OK: nothing was attached.
    esp_err_t err = ESP_ERR_INVALID_STATE;

    for (int i = 0; i < FREE_ALL_TRIES && err == ESP_ERR_INVALID_STATE; i++) {
        err = usb_host_device_free_all();
        if (err == ESP_ERR_INVALID_STATE) {
            vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY_MS));
        }
    }
    if (err == ESP_OK) {
        atomic_store(&s_all_free, true);
    } else if (err != ESP_ERR_NOT_FINISHED) {
        ESP_LOGE(TAG, "usb_host_device_free_all: %s", esp_err_to_name(err));
    }
    if (xSemaphoreTake(s_lib_done, pdMS_TO_TICKS(LIB_DONE_WAIT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "the USB host did not stop in %d ms: left installed", LIB_DONE_WAIT_MS);
        return; // s_host_up stays: a later start does not install it twice
    }
    if (s_lib_result != ESP_OK) {
        ESP_LOGE(TAG, "usb_host_uninstall: %s", esp_err_to_name(s_lib_result));
    }
    s_host_up = false;
    ESP_LOGI(TAG, "USB host stopped");
}

bool usb_serial_host_running(void)
{
    return s_host_up && s_cdc_up;
}

/* ---- dongle ---- */

static const char *driver_label(usb_serial_driver_t d)
{
    const char *name = usb_serial_driver_name(d);

    return name ? name : "none";
}

// Line coding (baud, 8N1) with read-back, then flow control. False when it closed the dongle.
static bool serial_setup(const usb_serial_settings_t *s, usb_serial_dongle_t *st)
{
    cdc_acm_line_coding_t lc = {.dwDTERate = s->baud, .bCharFormat = 0, .bParityType = 0, .bDataBits = 8};
    esp_err_t err = cdc_acm_host_line_coding_set(s_hdl, &lc);

    st->error[0] = '\0';
    if (err != ESP_OK) {
        snprintf(st->error, sizeof(st->error), "baud %lu not accepted: %s", (unsigned long)s->baud,
                 esp_err_to_name(err));
        usb_serial_dongle_close();
        st->open = false;
        st->flow_on = false;
        return false;
    }
    cdc_acm_line_coding_t back = {0};

    err = cdc_acm_host_line_coding_get(s_hdl, &back);
    if (err == ESP_OK) { // CH34x cannot read it back (ESP_ERR_NOT_SUPPORTED): nothing to compare
        const usb_serial_baud_t v = usb_serial_baud_verdict(s->baud, back.dwDTERate);

        if (v != USB_SERIAL_BAUD_EXACT) {
            snprintf(st->error, sizeof(st->error), "baud %lu read back as %lu", (unsigned long)s->baud,
                     (unsigned long)back.dwDTERate);
            ESP_LOGW(TAG, "%s", st->error);
        }
        if (v == USB_SERIAL_BAUD_FAR) {
            usb_serial_dongle_close();
            st->open = false;
            st->flow_on = false;
            return false;
        }
    }
    if (s_driver == USB_SERIAL_DRV_CP210X) {
        uint8_t payload[USB_SERIAL_CP210X_FLOW_LEN];

        usb_serial_cp210x_flow(s->flow_control, payload);
        err = cdc_acm_host_send_custom_request(s_hdl, CP210X_REQ_OUT, CP210X_SET_FLOW, 0, DATA_INTERFACE,
                                               sizeof(payload), payload);
        if (err != ESP_OK) {
            snprintf(st->error, sizeof(st->error), "flow control failed: %s", esp_err_to_name(err));
            st->flow_on = false;
        } else {
            st->flow_on = s->flow_control;
        }
    } else {
        st->flow_on = false;
        if (s->flow_control) {
            snprintf(st->error, sizeof(st->error), "flow control not supported by %s", driver_label(s_driver));
        }
    }
    return true;
}

esp_err_t usb_serial_dongle_open(const usb_serial_dev_t *dev, const usb_serial_settings_t *s, usb_serial_dongle_t *st)
{
    ESP_RETURN_ON_FALSE(usb_serial_host_running(), ESP_ERR_INVALID_STATE, TAG, "USB host not running");
    ESP_RETURN_ON_FALSE(!s_hdl, ESP_ERR_INVALID_STATE, TAG, "a dongle is already open");
    const cdc_acm_host_device_config_t cfg = {
        .connection_timeout_ms = OPEN_TIMEOUT_MS,
        .out_buffer_size = USB_BUF_SIZE,
        .in_buffer_size = USB_BUF_SIZE,
        .event_cb = event_cb,
        .data_cb = data_cb,
        .user_arg = NULL,
    };
    cdc_acm_dev_hdl_t hdl = NULL;
    esp_err_t err;

    switch (dev->driver) {
    case USB_SERIAL_DRV_CP210X:
        err = cp210x_vcp_open(dev->pid, DATA_INTERFACE, &cfg, &hdl);
        break;
    case USB_SERIAL_DRV_CH34X:
        err = ch34x_vcp_open(dev->pid, DATA_INTERFACE, &cfg, &hdl);
        break;
    case USB_SERIAL_DRV_CDC_ACM: {
        const cdc_acm_host_open_config_t open_cfg = {
            .vid = dev->vid,
            .pid = dev->pid,
            .interface_idx = 0,
            .dev_addr = dev->addr,
            .connection_timeout_ms = OPEN_TIMEOUT_MS,
            .out_buffer_size = USB_BUF_SIZE,
            .in_buffer_size = USB_BUF_SIZE,
            .event_cb = event_cb,
            .data_cb = data_cb,
            .user_arg = NULL,
        };

        err = cdc_acm_host_open_v2(&open_cfg, &hdl);
        break;
    }
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }
    st->open = false;
    st->flow_on = false;
    if (err != ESP_OK) {
        snprintf(st->error, sizeof(st->error), "open failed: %s", esp_err_to_name(err));
        ESP_LOGW(TAG, "%04x:%04x %s", dev->vid, dev->pid, st->error);
        return err;
    }
    s_hdl = hdl;
    s_driver = dev->driver;
    st->open = true;
    if (!serial_setup(s, st)) {
        return ESP_FAIL;
    }
    // DTR and RTS together, both asserted, in one request (one SET_MHS on CP210x): the auto-reset circuit never sees
    // the EN/IO0 pulse pattern. Never toggled afterwards.
    err = cdc_acm_host_set_control_line_state(s_hdl, true, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "DTR/RTS: %s", esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "%04x:%04x open (%s, %lu baud)", dev->vid, dev->pid, driver_label(dev->driver),
             (unsigned long)s->baud);
    return ESP_OK;
}

void usb_serial_dongle_configure(const usb_serial_settings_t *s, usb_serial_dongle_t *st)
{
    if (s_hdl) {
        serial_setup(s, st);
    }
}

void usb_serial_dongle_close(void)
{
    if (!s_hdl) {
        return;
    }
    usb_serial_rx_reset(false);
    const esp_err_t err = cdc_acm_host_close(s_hdl);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "cdc_acm_host_close: %s", esp_err_to_name(err));
    }
    s_hdl = NULL;
    s_driver = USB_SERIAL_DRV_NONE;
}

bool usb_serial_dongle_is_open(void)
{
    return s_hdl != NULL;
}

bool usb_serial_dongle_is(const void *hdl)
{
    return hdl && hdl == (const void *)s_hdl;
}

esp_err_t usb_serial_dongle_write(const uint8_t *data, size_t n)
{
    ESP_RETURN_ON_FALSE(s_hdl, ESP_ERR_INVALID_STATE, TAG, "no dongle");
    return cdc_acm_host_data_tx_blocking(s_hdl, data, n, 100);
}

bool usb_serial_dev_attached(uint8_t addr)
{
    uint8_t list[8];
    int n = 0;

    if (usb_host_device_addr_list_fill(sizeof(list), list, &n) != ESP_OK) {
        return true; // unknown: keep what the status shows
    }
    for (int i = 0; i < n; i++) {
        if (list[i] == addr) {
            return true;
        }
    }
    return false;
}

/* ---- dongle -> client ring ---- */

void usb_serial_rx_reset(bool accept)
{
    taskENTER_CRITICAL(&s_rx_mux);
    if (!s_rx.data) {
        usb_serial_ring_init(&s_rx, s_rx_storage, sizeof(s_rx_storage));
    }
    usb_serial_ring_clear(&s_rx);
    s_rx_accept = accept;
    s_rx_overflow = false;
    s_rx_first_us = 0;
    taskEXIT_CRITICAL(&s_rx_mux);
}

size_t usb_serial_rx_pending(int64_t *first_us)
{
    taskENTER_CRITICAL(&s_rx_mux);
    const size_t len = s_rx.len;

    *first_us = s_rx_first_us;
    taskEXIT_CRITICAL(&s_rx_mux);
    return len;
}

// The callback only appends behind the tail, so the head run stays valid outside the lock until consumed.
size_t usb_serial_rx_peek(const uint8_t **p)
{
    taskENTER_CRITICAL(&s_rx_mux);
    const size_t run = s_rx.len < s_rx.cap - s_rx.head ? s_rx.len : s_rx.cap - s_rx.head;

    *p = s_rx.data + s_rx.head;
    taskEXIT_CRITICAL(&s_rx_mux);
    return run;
}

void usb_serial_rx_consume(size_t n)
{
    taskENTER_CRITICAL(&s_rx_mux);
    if (n > s_rx.len) {
        n = s_rx.len;
    }
    s_rx.head = (s_rx.head + n) % s_rx.cap;
    s_rx.len -= n;
    if (s_rx.len > 0) {
        s_rx_first_us = esp_timer_get_time(); // the rest waits again from now: retried within the coalescing delay
    }
    taskEXIT_CRITICAL(&s_rx_mux);
}
