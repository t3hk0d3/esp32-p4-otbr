// USB serial proxy: the stored settings and the owner task. The owner task alone opens and closes the dongle and the
// sockets; the HTTP handlers and the USB callbacks post messages to it (spec section 3, Board side).
#include "usb_serial.h"

#include <errno.h>
#include <string.h>

#include "board_cores.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "netcfg.h"
#include "netcfg_policy.h"
#include "nvs.h"
#include "usb_serial_priv.h"

static const char *TAG = "usb_serial";

#define NVS_NAMESPACE "usb_serial"
#define KEY_ENABLED "enabled"
#define KEY_PORT "port"
#define KEY_BAUD "baud"
#define KEY_FLOW "flow"

#define OWNER_PRIORITY 6
#define OWNER_STACK 6144
#define QUEUE_LEN 8
#define SELECT_MS 5
#define FLUSH_BYTES 1024
#define FLUSH_AGE_US 5000
#define CLIENT_CHUNK 512
#define CONFIRM_WAIT_MS 5000
#define ATTACH_POLL_US 1000000 // how often an unsupported or closed device is checked for still being there

static QueueHandle_t s_queue;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static usb_serial_settings_t s_stored; // under s_mux
static usb_serial_status_t s_snap;     // under s_mux: the owner's last published status
static int64_t s_snap_since_us;        // under s_mux: when the client connected

// Owner task only.
static usb_serial_settings_t s_cur;
static bool s_running;
static int s_listen_fd = -1;
static int s_client_fd = -1;
static int64_t s_client_since_us;
static usb_serial_dev_t s_dev;
static usb_serial_status_t s_st;
static int64_t s_attach_check_us;

bool usb_serial_post(const usb_serial_msg_t *m)
{
    return s_queue && xQueueSend(s_queue, m, 0) == pdTRUE;
}

static void publish(void)
{
    taskENTER_CRITICAL(&s_mux);
    s_snap = s_st;
    s_snap_since_us = s_client_since_us;
    taskEXIT_CRITICAL(&s_mux);
}

/* ---- NVS ---- */

static void load_settings(usb_serial_settings_t *s)
{
    nvs_handle_t nvs;

    usb_serial_settings_default(s);
    const esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);

    if (err != ESP_OK) {
        if (err != ESP_ERR_NVS_NOT_FOUND) { // not found: nothing stored yet
            ESP_LOGE(TAG, "reading the settings failed (%s): defaults", esp_err_to_name(err));
        }
        return;
    }
    uint8_t u8;
    uint16_t u16;
    uint32_t u32;

    if (nvs_get_u8(nvs, KEY_ENABLED, &u8) == ESP_OK) {
        s->enabled = u8 != 0;
    }
    if (nvs_get_u16(nvs, KEY_PORT, &u16) == ESP_OK) {
        s->port = u16;
    }
    if (nvs_get_u32(nvs, KEY_BAUD, &u32) == ESP_OK) {
        s->baud = u32;
    }
    if (nvs_get_u8(nvs, KEY_FLOW, &u8) == ESP_OK) {
        s->flow_control = u8 != 0;
    }
    nvs_close(nvs);
    if (usb_serial_settings_check(s) != USB_SERIAL_OK) {
        ESP_LOGE(TAG, "stored settings are invalid: defaults");
        usb_serial_settings_default(s);
    }
}

static esp_err_t store_settings(const usb_serial_settings_t *s)
{
    nvs_handle_t nvs;

    ESP_RETURN_ON_ERROR(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs), TAG, "nvs_open");
    esp_err_t err = nvs_set_u8(nvs, KEY_ENABLED, s->enabled ? 1 : 0);

    err = err == ESP_OK ? nvs_set_u16(nvs, KEY_PORT, s->port) : err;
    err = err == ESP_OK ? nvs_set_u32(nvs, KEY_BAUD, s->baud) : err;
    err = err == ESP_OK ? nvs_set_u8(nvs, KEY_FLOW, s->flow_control ? 1 : 0) : err;
    err = err == ESP_OK ? nvs_commit(nvs) : err;
    nvs_close(nvs);
    ESP_RETURN_ON_ERROR(err, TAG, "storing the settings");
    return ESP_OK;
}

/* ---- owner: client ---- */

static void close_client(void)
{
    if (s_client_fd < 0) {
        return;
    }
    close(s_client_fd);
    s_client_fd = -1;
    memset(&s_st.client, 0, sizeof(s_st.client));
    s_client_since_us = 0;
    usb_serial_rx_reset(false);
    ESP_LOGI(TAG, "client closed");
}

static void close_listener(void)
{
    if (s_listen_fd >= 0) {
        close(s_listen_fd);
        s_listen_fd = -1;
    }
}

static void accept_one(void)
{
    struct sockaddr_storage sa;
    socklen_t len = sizeof(sa);
    const int fd = accept(s_listen_fd, (struct sockaddr *)&sa, &len);

    if (fd < 0) {
        return;
    }
    if (usb_serial_accept(s_client_fd >= 0) == USB_SERIAL_REFUSE ||
            !netcfg_socket_allowed(fd, NETCFG_SVC_USB_SERIAL)) {
        usb_serial_tcp_refuse(fd);
        s_st.refused_connections++;
        ESP_LOGI(TAG, "connection refused (%lu so far)", (unsigned long)s_st.refused_connections);
        return;
    }
    usb_serial_tcp_client_setup(fd, s_st.client.peer, sizeof(s_st.client.peer));
    s_client_fd = fd;
    s_st.client.present = true;
    s_client_since_us = esp_timer_get_time();
    usb_serial_rx_reset(usb_serial_dongle_is_open()); // a new session starts clean
    ESP_LOGI(TAG, "client %s connected", s_st.client.peer);
}

static void read_client(void)
{
    uint8_t buf[CLIENT_CHUNK];
    const int n = recv(s_client_fd, buf, sizeof(buf), MSG_DONTWAIT);

    if (n > 0) {
        if (usb_serial_dongle_is_open() && usb_serial_dongle_write(buf, (size_t)n) == ESP_OK) {
            s_st.bytes_to_dongle += (uint64_t)n;
        } else { // no dongle: never replayed into a radio plugged in later
            s_st.discarded_from_client += (uint64_t)n;
        }
        return;
    }
    if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
        close_client();
    }
}

static void flush_to_client(void)
{
    if (s_client_fd < 0) {
        return;
    }
    int64_t first_us;
    const size_t pending = usb_serial_rx_pending(&first_us);

    if (pending == 0 || (pending < FLUSH_BYTES && esp_timer_get_time() - first_us < FLUSH_AGE_US)) {
        return;
    }
    if (!usb_serial_tcp_flush(s_client_fd, &s_st.bytes_from_dongle)) {
        close_client();
    }
}

/* ---- owner: dongle ---- */

static void try_open(void)
{
    if (!s_st.dongle.present || s_st.dongle.driver == USB_SERIAL_DRV_NONE || usb_serial_dongle_is_open()) {
        return;
    }
    if (usb_serial_dongle_open(&s_dev, &s_cur, &s_st.dongle) == ESP_OK) {
        usb_serial_rx_reset(s_client_fd >= 0); // a new session starts clean
    }
}

static void forget_dongle(void)
{
    usb_serial_dongle_close();
    memset(&s_st.dongle, 0, sizeof(s_st.dongle));
}

static void on_new_dev(const usb_serial_dev_t *dev)
{
    if (!s_running) {
        return;
    }
    if (usb_serial_dongle_is_open()) { // only through a hub: the port has room for one device
        ESP_LOGW(TAG, "%04x:%04x ignored: one dongle at a time", dev->vid, dev->pid);
        return;
    }
    forget_dongle(); // one port: a device left closed is gone when another appears
    s_dev = *dev;
    s_st.dongle.present = true;
    s_st.dongle.vid = dev->vid;
    s_st.dongle.pid = dev->pid;
    s_st.dongle.driver = dev->driver;
    ESP_LOGI(TAG, "USB device %04x:%04x, driver %s", dev->vid, dev->pid,
             dev->driver == USB_SERIAL_DRV_NONE ? "none (unsupported)" : usb_serial_driver_name(dev->driver));
    s_attach_check_us = esp_timer_get_time();
    try_open();
}

static void on_gone(const void *hdl)
{
    if (!usb_serial_dongle_is(hdl)) {
        return; // already closed (a stop, a far-off baud rate)
    }
    ESP_LOGI(TAG, "dongle unplugged");
    close_client();
    forget_dongle();
}

// A device that is not open sends no unplug event: look it up now and then.
static void check_attached(void)
{
    if (!s_running || !s_st.dongle.present || usb_serial_dongle_is_open()) {
        return;
    }
    const int64_t now = esp_timer_get_time();

    if (now - s_attach_check_us < ATTACH_POLL_US) {
        return;
    }
    s_attach_check_us = now;
    if (!usb_serial_dev_attached(s_dev.addr)) {
        ESP_LOGI(TAG, "USB device %04x:%04x gone", s_dev.vid, s_dev.pid);
        memset(&s_st.dongle, 0, sizeof(s_st.dongle));
    }
}

/* ---- owner: start, stop, settings ---- */

static void start(void)
{
    const esp_err_t err = usb_serial_host_start();

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "USB host start: %s", esp_err_to_name(err));
    }
    s_listen_fd = usb_serial_tcp_listen(s_cur.port);
    s_running = true;
}

// The shutdown order: stop accepting, close the client, then the USB half (usb_host_glue.c).
static void stop(void)
{
    close_listener();
    close_client();
    usb_serial_host_stop();
    memset(&s_st.dongle, 0, sizeof(s_st.dongle));
    usb_serial_rx_reset(false);
    s_running = false;
}

static void apply(const usb_serial_settings_t *next)
{
    const usb_serial_settings_t old = s_cur;

    s_cur = *next;
    if (!s_running) {
        if (s_cur.enabled) {
            start();
        }
        return;
    }
    if (!s_cur.enabled) {
        stop();
        return;
    }
    if (s_cur.port != old.port) {
        close_client();
        close_listener();
        s_listen_fd = usb_serial_tcp_listen(s_cur.port);
    }
    if (s_cur.baud != old.baud || s_cur.flow_control != old.flow_control) {
        if (usb_serial_dongle_is_open()) {
            usb_serial_dongle_configure(&s_cur, &s_st.dongle);
            if (!usb_serial_dongle_is_open()) { // the new rate read back far off
                close_client();
            }
        } else {
            try_open(); // a dongle left closed by the old settings gets another chance
        }
    }
}

static void handle(const usb_serial_msg_t *m)
{
    switch (m->type) {
    case USB_SERIAL_MSG_APPLY:
        apply(&m->settings);
        break;
    case USB_SERIAL_MSG_KICK:
        close_client();
        break;
    case USB_SERIAL_MSG_NEW_DEV:
        on_new_dev(&m->dev);
        break;
    case USB_SERIAL_MSG_GONE:
        on_gone(m->hdl);
        break;
    case USB_SERIAL_MSG_OVERFLOW:
        if (s_client_fd >= 0) {
            ESP_LOGW(TAG, "the client cannot keep up with the dongle: disconnected");
            s_st.overflow_disconnects++;
            close_client();
        }
        usb_serial_rx_reset(false);
        break;
    }
    if (m->done) {
        publish(); // the caller answers with the status after the change
        xSemaphoreGive(m->done);
    }
}

static void owner_task(void *arg)
{
    usb_serial_msg_t m;

    publish();
    for (;;) {
        if (!s_running) {
            if (xQueueReceive(s_queue, &m, portMAX_DELAY) == pdTRUE) {
                handle(&m);
                publish();
            }
            continue;
        }
        fd_set rd;
        int maxfd = -1;

        FD_ZERO(&rd);
        if (s_listen_fd >= 0) {
            FD_SET(s_listen_fd, &rd);
            maxfd = s_listen_fd;
        }
        if (s_client_fd >= 0) {
            FD_SET(s_client_fd, &rd);
            maxfd = s_client_fd > maxfd ? s_client_fd : maxfd;
        }
        if (maxfd >= 0) {
            struct timeval tv = {.tv_sec = 0, .tv_usec = SELECT_MS * 1000};
            const int ready = select(maxfd + 1, &rd, NULL, NULL, &tv);

            if (ready > 0) {
                if (s_listen_fd >= 0 && FD_ISSET(s_listen_fd, &rd)) {
                    accept_one();
                }
                if (s_client_fd >= 0 && FD_ISSET(s_client_fd, &rd)) {
                    read_client();
                }
            } else if (ready < 0) {
                vTaskDelay(pdMS_TO_TICKS(SELECT_MS));
            }
        } else { // no listener (the port could not be bound): just the messages
            if (xQueueReceive(s_queue, &m, pdMS_TO_TICKS(SELECT_MS)) == pdTRUE) {
                handle(&m);
            }
        }
        while (xQueueReceive(s_queue, &m, 0) == pdTRUE) {
            handle(&m);
        }
        flush_to_client();
        check_attached();
        publish();
    }
}

/* ---- API ---- */

esp_err_t usb_serial_init(void)
{
    ESP_RETURN_ON_FALSE(!s_queue, ESP_ERR_INVALID_STATE, TAG, "already started");
    usb_serial_settings_t s;

    load_settings(&s);
    taskENTER_CRITICAL(&s_mux);
    s_stored = s;
    taskEXIT_CRITICAL(&s_mux);
    s_cur = s;
    s_cur.enabled = false; // not running yet: the APPLY posted below starts it when stored so
    usb_serial_rx_reset(false);
    QueueHandle_t q = xQueueCreate(QUEUE_LEN, sizeof(usb_serial_msg_t));

    ESP_RETURN_ON_FALSE(q, ESP_ERR_NO_MEM, TAG, "queue");
    s_queue = q;
    if (xTaskCreatePinnedToCore(owner_task, "usb_serial", OWNER_STACK, NULL, OWNER_PRIORITY, NULL, MGMT_CORE) !=
            pdPASS) {
        s_queue = NULL;
        vQueueDelete(q);
        return ESP_ERR_NO_MEM;
    }
    if (s.enabled) {
        const usb_serial_msg_t m = {.type = USB_SERIAL_MSG_APPLY, .settings = s};

        usb_serial_post(&m);
    }
    ESP_LOGI(TAG, "%s, port %u, %lu baud, flow control %s", s.enabled ? "on" : "off", s.port,
             (unsigned long)s.baud, s.flow_control ? "on" : "off");
    return ESP_OK;
}

void usb_serial_get_settings(usb_serial_settings_t *out)
{
    if (!s_queue) {
        usb_serial_settings_default(out);
        return;
    }
    taskENTER_CRITICAL(&s_mux);
    *out = s_stored;
    taskEXIT_CRITICAL(&s_mux);
}

// Posts `m` and waits for the owner to handle it. The semaphore is not deleted on a timeout: the owner gives it later.
static esp_err_t post_and_wait(usb_serial_msg_t *m)
{
    m->done = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(m->done, ESP_ERR_NO_MEM, TAG, "semaphore");
    if (xQueueSend(s_queue, m, pdMS_TO_TICKS(CONFIRM_WAIT_MS)) != pdTRUE) {
        vSemaphoreDelete(m->done);
        return ESP_ERR_TIMEOUT;
    }
    if (xSemaphoreTake(m->done, pdMS_TO_TICKS(CONFIRM_WAIT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "the owner task did not confirm in %d ms", CONFIRM_WAIT_MS);
        return ESP_ERR_TIMEOUT; // leaked on purpose: the owner still gives it
    }
    vSemaphoreDelete(m->done);
    return ESP_OK;
}

esp_err_t usb_serial_apply(const usb_serial_settings_t *s)
{
    ESP_RETURN_ON_FALSE(s_queue, ESP_ERR_INVALID_STATE, TAG, "not started");
    if (usb_serial_settings_check(s) != USB_SERIAL_OK) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(store_settings(s), TAG, "store");
    taskENTER_CRITICAL(&s_mux);
    s_stored = *s;
    taskEXIT_CRITICAL(&s_mux);
    usb_serial_msg_t m = {.type = USB_SERIAL_MSG_APPLY, .settings = *s};

    return post_and_wait(&m);
}

esp_err_t usb_serial_kick(void)
{
    ESP_RETURN_ON_FALSE(s_queue, ESP_ERR_INVALID_STATE, TAG, "not started");
    usb_serial_msg_t m = {.type = USB_SERIAL_MSG_KICK};

    return post_and_wait(&m);
}

void usb_serial_get_status(usb_serial_status_t *out)
{
    taskENTER_CRITICAL(&s_mux);
    *out = s_snap;
    const int64_t since_us = s_snap_since_us;
    taskEXIT_CRITICAL(&s_mux);
    if (out->client.present && since_us > 0) {
        out->client.since_s = (uint32_t)((esp_timer_get_time() - since_us) / 1000000);
    }
}
