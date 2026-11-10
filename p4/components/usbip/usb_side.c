// USB/IP esp-usb side (spec sections 2, 3, 5, 8.2): the USB host (installed here, never uninstalled), the usbip
// client and its USB task, the one opened device, and the engine's ops on esp-usb.
//
// Tasks: the usb_lib task handles the host library's events; the usbip_usb task handles the client's events (new
// device, device gone, transfer completions) and only posts messages to the network task; the network task calls
// everything else here (open, close, strings, the ops). Transfer callbacks never touch sockets or the engine.
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
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
#include "usb/usb_helpers.h"
#include "usb/usb_host.h"
#include "usb/usb_types_ch9.h"
#include "usbip_priv.h"

static const char *TAG = "usbip";

#define LIB_TASK_PRIORITY 7
#define LIB_TASK_STACK 4096
#define LIB_POLL_MS 50
#define USB_TASK_PRIORITY 7 // as radio_mux's CDC driver task: the callbacks only post
#define USB_TASK_STACK 4096
#define USB_POLL_MS 50 // also the period of the root port power-on check
#define CLIENT_EVENT_MSGS 8
#define ADDRS_MAX 8
// A flushed transfer completes once the USB task handles the client's events; interface release waits that long.
#define IFACE_RELEASE_WAIT_MS 1000
// The root port stays off at least this long, so the disconnect registers before power returns.
#define POWER_OFF_MIN_MS 100
#define POWER_OFF_WARN_MS 5000
#define IFACE_BITMAP_WORDS (256 / 32)
#define ASCII_FIRST 0x20
#define ASCII_LAST 0x7E

// Linux enum usb_device_speed (spec 6.2).
#define LINUX_SPEED_LOW 1
#define LINUX_SPEED_FULL 2
#define LINUX_SPEED_HIGH 3

static QueueHandle_t s_events;
static void (*s_wake)(void);
static usb_host_client_handle_t s_client;
static bool s_started;

// The address of the last enumerated device (0: none). Written by the USB task, read by the network task.
static _Atomic uint8_t s_new_addr;

// Network task only: the opened device and its interfaces claimed in esp-usb (a bit stays set after a release that
// failed; the next claim or the close releases it again).
static usb_device_handle_t s_dev;
static _Atomic uint8_t s_dev_addr; // also read by the USB task (DEV_GONE)
static uint32_t s_claimed[IFACE_BITMAP_WORDS];
static atomic_bool s_dev_open; // read by the USB task (root port power-on)
// Set by the USB task when esp-usb reports the open device gone, cleared by usbip_usb_open. Once the hub has seen the
// disconnect, its port is in recovery: powering it off then would make esp-usb abort when the device is freed
// (root_port_recycle finds an unpowered port), so port_power_cycle leaves the port to esp-usb's own recovery.
static atomic_bool s_gone;

// Network task only: the live DMA transfers, found by their data buffer (alloc returns the buffer, free and submit get
// it back). Bounded, so the events queue can always take every completion.
static usb_transfer_t *s_xfers[USBIP_USB_XFERS_MAX];

// Root port power (spec 5): turned off by the network task, on again by the USB task once the device is freed.
static SemaphoreHandle_t s_power_lock;
static bool s_power_off;      // under s_power_lock
static int64_t s_power_off_us; // under s_power_lock
static bool s_power_warned;   // USB task only

/* ---- messages to the network task (USB task) ---- */

static void post(const usbip_event_t *ev)
{
    // Blocks rather than drops: every submit gets exactly one completion (usbip_engine.h). The queue holds
    // USBIP_USB_EVENTS_LEN messages, more than there can be live transfers.
    xQueueSend(s_events, ev, portMAX_DELAY);
    if (s_wake != NULL) {
        s_wake();
    }
}

static void post_dev(int type)
{
    const usbip_event_t ev = {.type = type};

    post(&ev);
}

static usbip_xfer_status_t map_status(usb_transfer_status_t st)
{
    switch (st) {
    case USB_TRANSFER_STATUS_COMPLETED:
        return USBIP_XS_COMPLETED;
    case USB_TRANSFER_STATUS_STALL:
        return USBIP_XS_STALL;
    case USB_TRANSFER_STATUS_OVERFLOW:
        return USBIP_XS_OVERFLOW;
    case USB_TRANSFER_STATUS_NO_DEVICE:
        return USBIP_XS_NO_DEVICE;
    case USB_TRANSFER_STATUS_CANCELED:
        return USBIP_XS_CANCELED;
    case USB_TRANSFER_STATUS_ERROR:
    case USB_TRANSFER_STATUS_TIMED_OUT: // never reported (spec 2)
    case USB_TRANSFER_STATUS_SKIPPED:   // isochronous only
    default:
        return USBIP_XS_ERROR;
    }
}

// Reads the transfer and posts; the transfer is left alone afterwards (the network task may free it at once).
static void xfer_cb(usb_transfer_t *t)
{
    // NO_DEVICE comes only when the root port lost its device (no hubs). esp-usb runs these callbacks before the
    // client's DEV_GONE message, and the engine ends the session on this completion: mark the device gone first, so
    // port_power_cycle does not power off a port that is already recovering.
    if (t->status == USB_TRANSFER_STATUS_NO_DEVICE) {
        atomic_store(&s_gone, true);
    }
    const usbip_event_t ev = {
        .type = USBIP_EV_XFER_DONE,
        .x = t->context,
        .st = map_status(t->status),
        .actual = t->actual_num_bytes > 0 ? (size_t)t->actual_num_bytes : 0,
    };

    post(&ev);
}

static void client_event_cb(const usb_host_client_event_msg_t *msg, void *arg)
{
    (void)arg;
    switch (msg->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV:
        atomic_store(&s_new_addr, msg->new_dev.address);
        post_dev(USBIP_EV_DEV_NEW);
        break;
    case USB_HOST_CLIENT_EVENT_DEV_GONE: { // the device we hold (only owners get it)
        uint8_t addr = atomic_load(&s_dev_addr);

        atomic_store(&s_gone, true);
        // Reported here; a later DEV_REMOVED for its address is not a second GONE.
        if (addr != 0) {
            atomic_compare_exchange_strong(&s_new_addr, &addr, 0);
        }
        post_dev(USBIP_EV_DEV_GONE);
        break;
    }
    case USB_HOST_CLIENT_EVENT_DEV_REMOVED: { // a device we do not hold (unsupported, or its open failed)
        uint8_t addr = msg->dev_removed.address;

        if (addr != 0 && atomic_compare_exchange_strong(&s_new_addr, &addr, 0)) {
            post_dev(USBIP_EV_DEV_GONE);
        }
        break;
    }
    default:
        break;
    }
}

/* ---- root port power ---- */

static void power_restore_check(void)
{
    xSemaphoreTake(s_power_lock, portMAX_DELAY);
    const bool off = s_power_off;
    const int64_t since_ms = (esp_timer_get_time() - s_power_off_us) / 1000;

    if (!off || since_ms < POWER_OFF_MIN_MS) {
        xSemaphoreGive(s_power_lock);
        return;
    }
    usb_host_lib_info_t info = {0};

    // Power on only once the old device is closed and freed: its port must have recovered first (hub.c), or the
    // disconnect is taken for one before enumeration and the stale device is never freed. A refusal (the port is
    // still recovering) is retried on the next poll.
    if (!atomic_load(&s_dev_open) && usb_host_lib_info(&info) == ESP_OK && info.num_devices == 0 &&
            usb_host_lib_set_root_port_power(true) == ESP_OK) {
        s_power_off = false;
        s_power_warned = false;
        ESP_LOGI(TAG, "root port on again after %lld ms", (long long)since_ms);
    } else if (since_ms >= POWER_OFF_WARN_MS && !s_power_warned) {
        s_power_warned = true;
        ESP_LOGW(TAG, "root port still off after %lld ms: %s", (long long)since_ms,
                 atomic_load(&s_dev_open) ? "the device is still open" : "waiting for the device to be freed");
    }
    xSemaphoreGive(s_power_lock);
}

static void port_power_cycle(void *ctx)
{
    (void)ctx;
    if (s_dev == NULL || atomic_load(&s_gone)) {
        // Gone already: the hub recovers and powers the port itself, and the next device enumerates as usual.
        ESP_LOGI(TAG, "root port left on: the device is gone");
        return;
    }
    xSemaphoreTake(s_power_lock, portMAX_DELAY);
    esp_err_t err = usb_host_lib_set_root_port_power(false);

    if (err == ESP_OK) {
        s_power_off = true;
        s_power_off_us = esp_timer_get_time();
        ESP_LOGI(TAG, "root port off (device reset)");
    } else {
        ESP_LOGW(TAG, "root port off: %s", esp_err_to_name(err));
    }
    xSemaphoreGive(s_power_lock);
    usb_host_client_unblock(s_client); // the USB task's power-on check runs soon
}

/* ---- tasks ---- */

typedef struct {
    SemaphoreHandle_t done;
    esp_err_t err;
} lib_start_t;

// As radio_mux's usb_transport.c: installs the host from NET_CORE (its interrupt is allocated on the calling core),
// then handles the host's events for good.
static void lib_task(void *arg)
{
    lib_start_t *start = arg;
    const usb_host_config_t host_cfg = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LOWMED,
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

static void usb_task(void *arg)
{
    (void)arg;
    for (;;) {
        usb_host_client_handle_events(s_client, pdMS_TO_TICKS(USB_POLL_MS));
        power_restore_check();
    }
}

void usbip_usb_set_wake(void (*wake)(void))
{
    s_wake = wake;
}

esp_err_t usbip_usb_start(QueueHandle_t events)
{
    ESP_RETURN_ON_FALSE(events != NULL, ESP_ERR_INVALID_ARG, TAG, "usb start: no events queue");
    ESP_RETURN_ON_FALSE(!s_started, ESP_ERR_INVALID_STATE, TAG, "usb start: already started");
    s_events = events;
    s_power_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_power_lock != NULL, ESP_ERR_NO_MEM, TAG, "power lock");

    lib_start_t start = {.done = xSemaphoreCreateBinary(), .err = ESP_FAIL};

    ESP_RETURN_ON_FALSE(start.done != NULL, ESP_ERR_NO_MEM, TAG, "usb_lib start semaphore");
    if (xTaskCreatePinnedToCore(lib_task, "usb_lib", LIB_TASK_STACK, &start, LIB_TASK_PRIORITY, NULL, NET_CORE) !=
            pdPASS) {
        vSemaphoreDelete(start.done);
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(start.done, portMAX_DELAY);
    vSemaphoreDelete(start.done);
    ESP_RETURN_ON_ERROR(start.err, TAG, "usb_host_install");

    const usb_host_client_config_t client_cfg = {
        .is_synchronous = false,
        .max_num_event_msg = CLIENT_EVENT_MSGS,
        .flags = {.notify_dev_removed = 1}, // a device never opened (unsupported) still reports its unplug
        .async = {.client_event_callback = client_event_cb, .callback_arg = NULL},
    };

    ESP_RETURN_ON_ERROR(usb_host_client_register(&client_cfg, &s_client), TAG, "usb_host_client_register");
    // A device enumerated before the client existed sent its NEW_DEV to nobody.
    uint8_t addrs[ADDRS_MAX];
    int n = 0;

    if (usb_host_device_addr_list_fill(ADDRS_MAX, addrs, &n) == ESP_OK && n > 0) {
        atomic_store(&s_new_addr, addrs[n - 1]);
        post_dev(USBIP_EV_DEV_NEW);
    }
    if (xTaskCreatePinnedToCore(usb_task, "usbip_usb", USB_TASK_STACK, NULL, USB_TASK_PRIORITY, NULL, NET_CORE) !=
            pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    s_started = true;
    ESP_LOGI(TAG, "USB host started (USB/IP owns the port)");
    return ESP_OK;
}

/* ---- transfers (network task) ---- */

// The slot of the live transfer whose data buffer is buf; -1 when none.
static int xfer_slot(const void *buf)
{
    if (buf == NULL) {
        return -1;
    }
    for (int i = 0; i < USBIP_USB_XFERS_MAX; i++) {
        if (s_xfers[i] != NULL && s_xfers[i]->data_buffer == buf) {
            return i;
        }
    }
    return -1;
}

// DMA: an esp-usb transfer's own data buffer. It is cache aligned and its size rounded up to the cache line, which
// the HCD's cache sync of the buffer relies on; plain heap memory would need the same care, and a transfer per submit
// would copy every buffer. The transfer is found again from the buffer (s_xfers) and freed with it.
static void *ops_alloc(void *ctx, size_t n, bool dma)
{
    (void)ctx;
    if (!dma) {
        return heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    }
    int slot = -1;

    for (int i = 0; i < USBIP_USB_XFERS_MAX && slot < 0; i++) {
        if (s_xfers[i] == NULL) {
            slot = i;
        }
    }
    if (slot < 0) {
        ESP_LOGW(TAG, "all %d DMA transfer slots in use: allocation refused", USBIP_USB_XFERS_MAX);
        return NULL;
    }
    usb_transfer_t *t = NULL;

    if (usb_host_transfer_alloc(n, 0, &t) != ESP_OK) {
        return NULL;
    }
    s_xfers[slot] = t;
    return t->data_buffer;
}

static void ops_free(void *ctx, void *p)
{
    (void)ctx;
    if (p == NULL) {
        return;
    }
    int slot = xfer_slot(p);

    if (slot >= 0) {
        usb_host_transfer_free(s_xfers[slot]);
        s_xfers[slot] = NULL;
    } else {
        heap_caps_free(p);
    }
}

static esp_err_t ops_submit(void *ctx, usbip_xfer_t *x)
{
    (void)ctx;
    int slot = xfer_slot(x->buf);

    ESP_RETURN_ON_FALSE(slot >= 0, ESP_ERR_INVALID_ARG, TAG, "submit: buffer not from alloc(dma)");
    ESP_RETURN_ON_FALSE(s_dev != NULL, ESP_ERR_INVALID_STATE, TAG, "submit: no device");
    usb_transfer_t *t = s_xfers[slot];

    ESP_RETURN_ON_FALSE(x->len <= t->data_buffer_size, ESP_ERR_INVALID_SIZE, TAG, "submit: %u bytes in a %u buffer",
                        (unsigned)x->len, (unsigned)t->data_buffer_size);
    t->device_handle = s_dev;
    t->bEndpointAddress = x->type == USB_TRANSFER_TYPE_CTRL ? 0 : x->ep;
    t->num_bytes = (int)x->len;
    t->flags = (x->flags & USBIP_URB_ZERO_PACKET) ? USB_TRANSFER_FLAG_ZERO_PACK : 0;
    t->timeout_ms = 0;
    t->callback = xfer_cb;
    t->context = x;
    x->native = t;
    if (x->type == USB_TRANSFER_TYPE_CTRL) {
        return usb_host_transfer_submit_control(s_client, t);
    }
    return usb_host_transfer_submit(t);
}

/* ---- endpoints and interfaces (network task) ---- */

static esp_err_t ops_ep_halt(void *ctx, uint8_t ep)
{
    (void)ctx;
    ESP_RETURN_ON_FALSE(s_dev != NULL, ESP_ERR_INVALID_STATE, TAG, "halt: no device");
    return usb_host_endpoint_halt(s_dev, ep);
}

static esp_err_t ops_ep_flush(void *ctx, uint8_t ep)
{
    (void)ctx;
    ESP_RETURN_ON_FALSE(s_dev != NULL, ESP_ERR_INVALID_STATE, TAG, "flush: no device");
    return usb_host_endpoint_flush(s_dev, ep);
}

static esp_err_t ops_ep_clear(void *ctx, uint8_t ep)
{
    (void)ctx;
    ESP_RETURN_ON_FALSE(s_dev != NULL, ESP_ERR_INVALID_STATE, TAG, "clear: no device");
    return usb_host_endpoint_clear(s_dev, ep);
}

static bool claimed_get(uint8_t iface)
{
    return (s_claimed[iface / 32] >> (iface % 32)) & 1;
}

static void claimed_set(uint8_t iface, bool on)
{
    if (on) {
        s_claimed[iface / 32] |= 1u << (iface % 32);
    } else {
        s_claimed[iface / 32] &= ~(1u << (iface % 32));
    }
}

// esp-usb refuses to release an interface while one of its endpoints has a transfer in flight or an event pending,
// and a flush only queues the canceled transfers for the USB task. Retries until they have come back (each lands on
// the events queue as USBIP_EV_XFER_DONE, for the network task to hand to the engine later), up to
// IFACE_RELEASE_WAIT_MS. A release that still fails keeps the interface marked claimed: the next claim of it or the
// close releases it again.
static esp_err_t iface_release(uint8_t iface)
{
    ESP_RETURN_ON_FALSE(s_dev != NULL, ESP_ERR_INVALID_STATE, TAG, "release: no device");
    const int64_t deadline = esp_timer_get_time() + (int64_t)IFACE_RELEASE_WAIT_MS * 1000;
    esp_err_t err;

    for (;;) {
        err = usb_host_interface_release(s_client, s_dev, iface);
        if (err != ESP_ERR_INVALID_STATE || esp_timer_get_time() >= deadline) {
            break;
        }
        vTaskDelay(1);
    }
    if (err == ESP_OK || err == ESP_ERR_NOT_FOUND) {
        claimed_set(iface, false);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "releasing interface %u: %s", iface, esp_err_to_name(err));
    }
    return err == ESP_ERR_NOT_FOUND ? ESP_OK : err;
}

static esp_err_t ops_iface_release(void *ctx, uint8_t iface)
{
    (void)ctx;
    return iface_release(iface);
}

static esp_err_t ops_iface_claim(void *ctx, uint8_t iface, uint8_t alt)
{
    (void)ctx;
    ESP_RETURN_ON_FALSE(s_dev != NULL, ESP_ERR_INVALID_STATE, TAG, "claim: no device");
    if (claimed_get(iface)) {
        ESP_RETURN_ON_ERROR(iface_release(iface), TAG, "interface %u still claimed", iface);
    }
    ESP_RETURN_ON_ERROR(usb_host_interface_claim(s_client, s_dev, iface, alt), TAG, "claiming interface %u alt %u",
                        iface, alt);
    claimed_set(iface, true);
    return ESP_OK;
}

static const usbip_engine_ops_t s_ops = {
    .submit = ops_submit,
    .ep_halt = ops_ep_halt,
    .ep_flush = ops_ep_flush,
    .ep_clear = ops_ep_clear,
    .iface_claim = ops_iface_claim,
    .iface_release = ops_iface_release,
    .port_power_cycle = port_power_cycle,
    .alloc = ops_alloc,
    .free = ops_free,
    // emit, end_session, now_ms: net.c
};

const usbip_engine_ops_t *usbip_usb_ops(void)
{
    return &s_ops;
}

/* ---- the device (network task) ---- */

static uint32_t linux_speed(usb_speed_t s)
{
    switch (s) {
    case USB_SPEED_LOW:
        return LINUX_SPEED_LOW;
    case USB_SPEED_FULL:
        return LINUX_SPEED_FULL;
    default:
        return LINUX_SPEED_HIGH;
    }
}

static void say(char *why, size_t cap, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

static void say(char *why, size_t cap, const char *fmt, ...)
{
    if (why == NULL || cap == 0) {
        return;
    }
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(why, cap, fmt, ap);
    va_end(ap);
}

// Claims every interface at alternate setting 0, then releases them all: the engine claims them per session.
static esp_err_t check_claims(const usb_config_desc_t *cd, char *why, size_t cap)
{
    const usb_standard_desc_t *d = (const usb_standard_desc_t *)cd;
    int off = 0;
    esp_err_t err = ESP_OK;

    while ((d = usb_parse_next_descriptor_of_type(d, cd->wTotalLength, USB_B_DESCRIPTOR_TYPE_INTERFACE, &off)) !=
            NULL) {
        const usb_intf_desc_t *intf = (const usb_intf_desc_t *)d;

        if (intf->bAlternateSetting != 0 || claimed_get(intf->bInterfaceNumber)) {
            continue;
        }
        err = ops_iface_claim(NULL, intf->bInterfaceNumber, 0);
        if (err != ESP_OK) {
            say(why, cap, "claiming interface %u failed: %s", intf->bInterfaceNumber, esp_err_to_name(err));
            break;
        }
    }
    for (int i = 0; i < 256; i++) {
        if (claimed_get((uint8_t)i)) {
            iface_release((uint8_t)i);
        }
    }
    return err;
}

esp_err_t usbip_usb_open(usbip_dev_rec_t *rec, const uint8_t **cfg, size_t *cfg_len, char *why, size_t cap)
{
    ESP_RETURN_ON_FALSE(rec != NULL && cfg != NULL && cfg_len != NULL, ESP_ERR_INVALID_ARG, TAG, "open: arguments");
    say(why, cap, "%s", "");
    if (!s_started || s_dev != NULL) {
        say(why, cap, "%s", s_started ? "a device is open already (close it first)" : "USB host not running");
        return ESP_ERR_INVALID_STATE;
    }
    const uint8_t addr = atomic_load(&s_new_addr);

    if (addr == 0) {
        say(why, cap, "%s", "no device");
        return ESP_ERR_NOT_FOUND;
    }
    usb_device_handle_t dev = NULL;

    // Before the open: an unplug right after it then sets s_gone and finds the address for DEV_GONE.
    atomic_store(&s_gone, false);
    s_dev_addr = addr;
    esp_err_t err = usb_host_device_open(s_client, addr, &dev);

    if (err != ESP_OK) {
        s_dev_addr = 0;
        say(why, cap, "the device failed to open: %s", esp_err_to_name(err));
        return err;
    }
    s_dev = dev;
    atomic_store(&s_dev_open, true);

    const usb_device_desc_t *dd = NULL;
    const usb_config_desc_t *cd = NULL;
    usb_device_info_t info;

    err = usb_host_get_device_descriptor(dev, &dd);
    if (err == ESP_OK) {
        err = usb_host_device_info(dev, &info);
    }
    if (err == ESP_OK) {
        err = usb_host_get_active_config_descriptor(dev, &cd);
    }
    if (err != ESP_OK || dd == NULL || cd == NULL) {
        say(why, cap, "the device's descriptors are unavailable: %s", esp_err_to_name(err != ESP_OK ? err : ESP_FAIL));
        usbip_usb_close();
        return err != ESP_OK ? err : ESP_FAIL;
    }
    err = usbip_core_dev_record((const uint8_t *)dd, (const uint8_t *)cd, cd->wTotalLength, info.dev_addr,
                                linux_speed(info.speed), rec, why, cap);
    if (err == ESP_ERR_INVALID_SIZE || err == ESP_ERR_INVALID_ARG) {
        say(why, cap, "%s", "malformed descriptors");
    }
    if (err == ESP_OK) {
        err = check_claims(cd, why, cap);
    }
    if (err != ESP_OK) {
        usbip_usb_close();
        return err;
    }
    *cfg = (const uint8_t *)cd;
    *cfg_len = cd->wTotalLength;
    ESP_LOGI(TAG, "device %04x:%04x at address %u opened", dd->idVendor, dd->idProduct, info.dev_addr);
    return ESP_OK;
}

void usbip_usb_close(void)
{
    if (s_dev == NULL) {
        return;
    }
    for (int i = 0; i < 256; i++) {
        if (claimed_get((uint8_t)i)) {
            iface_release((uint8_t)i);
        }
    }
    esp_err_t err = usb_host_device_close(s_client, s_dev);

    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
        // Kept: the next close tries again (open refuses meanwhile). A pending root port power-on waits for it.
        ESP_LOGE(TAG, "closing the device: %s", esp_err_to_name(err));
        return;
    }
    // s_new_addr stays: if the device is still there (an open that failed), its unplug comes as DEV_REMOVED and is
    // reported as USBIP_EV_DEV_GONE. A device that went while open cleared it on DEV_GONE.
    s_dev = NULL;
    s_dev_addr = 0;
    memset(s_claimed, 0, sizeof(s_claimed));
    atomic_store(&s_dev_open, false);
    usb_host_client_unblock(s_client); // a pending root port power-on can go ahead
}

// UTF-16LE string descriptor to ASCII; other characters as '?'.
static void str_ascii(const usb_str_desc_t *d, char *out, size_t cap)
{
    size_t n = 0;

    if (cap == 0) {
        return;
    }
    if (d != NULL && d->bLength > USB_STR_DESC_SIZE) {
        const size_t chars = (d->bLength - USB_STR_DESC_SIZE) / 2;

        for (size_t i = 0; i < chars && n + 1 < cap; i++) {
            const uint16_t c = d->wData[i];

            out[n++] = c >= ASCII_FIRST && c <= ASCII_LAST ? (char)c : '?';
        }
    }
    out[n] = '\0';
}

void usbip_usb_strings(char *manufacturer, char *product, char *serial, size_t cap)
{
    usb_device_info_t info = {0};
    const bool ok = s_dev != NULL && usb_host_device_info(s_dev, &info) == ESP_OK;

    str_ascii(ok ? info.str_desc_manufacturer : NULL, manufacturer, cap);
    str_ascii(ok ? info.str_desc_product : NULL, product, cap);
    str_ascii(ok ? info.str_desc_serial_num : NULL, serial, cap);
}
