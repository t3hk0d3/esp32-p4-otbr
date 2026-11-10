// Time sync runtime: the settings in NVS, and the timesync task that owns ESP-IDF's SNTP client (spec 5.1). Callbacks on
// lwIP's thread (the sync callback, the DHCP wrap in dhcp_wrap.c) and on the event loop (lost IP) only record and
// notify the task; only the task starts, stops and polls the client.

#include <string.h>
#include <sys/time.h>
#include <time.h>
#include "board_cores.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "netcfg.h"
#include "nvs.h"
#include "timesync_priv.h"
#include "timesync_rules.h"

static const char *TAG = "timesync";

#define NVS_NAMESPACE "timesync"
#define KEY_ENABLED "en"
#define KEY_MODE "mode"
#define KEY_SERVER "server"

#define TASK_STACK 3072
#define TASK_PRIORITY 2
#define UPKEEP_MS 10000 // state upkeep (reachability) between notifications
#define ANSWER_SETTLE_MS 50 // after an answer, before reading reachability (see the task loop)

#define BIT_CONFIG BIT0
#define BIT_DHCP BIT1
#define BIT_ANSWERED BIT2

// Everything below s_lock is read by timesync_get_status from any task and written by the task or the callbacks.
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static timesync_config_t s_cfg = {.enabled = true, .mode = TIMESYNC_MODE_AUTO};
static timesync_dhcp_t s_dhcp;
static timesync_servers_t s_servers; // in use; lwIP keeps pointers to these names while the client runs
static int64_t s_start_us;           // when the client was last (re)started
static int64_t s_last_sync_us = -1;  // the last answer since boot; -1 none
static bool s_answered_since_start;
static bool s_any_reachable;
static int8_t s_answered = -1;
static bool s_retry; // the task's: the last start failed and is tried again at the next upkeep

static TaskHandle_t s_task;

static void notify(uint32_t bits)
{
    if (s_task) {
        xTaskNotify(s_task, bits, eSetBits);
    }
}

void timesync_note_dhcp(uint8_t count, const uint32_t *addrs_net_order, timesync_iface_t iface)
{
    taskENTER_CRITICAL(&s_lock);
    timesync_rules_dhcp_capture(&s_dhcp, count, addrs_net_order, iface);
    taskEXIT_CRITICAL(&s_lock);
    notify(BIT_DHCP);
}

timesync_iface_t timesync_iface_of(const esp_netif_t *netif)
{
    const esp_netif_t *mgmt = netcfg_mgmt_netif(), *backbone = netcfg_backbone_netif();

    if (!netif) {
        return TIMESYNC_IFACE_NONE;
    }
    if (mgmt == backbone) {
        return netif == mgmt ? TIMESYNC_IFACE_SINGLE : TIMESYNC_IFACE_NONE;
    }
    if (netif == mgmt) {
        return TIMESYNC_IFACE_MGMT;
    }
    return netif == backbone ? TIMESYNC_IFACE_THREAD_BB : TIMESYNC_IFACE_NONE;
}

// Event loop task: a lost lease takes its NTP servers with it.
static void on_lost_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    const ip_event_got_ip_t *ev = data;

    timesync_note_dhcp(0, NULL, timesync_iface_of(ev ? ev->esp_netif : NULL));
}

// lwIP's thread: record the moment only.
static void on_sync(struct timeval *tv)
{
    taskENTER_CRITICAL(&s_lock);
    s_last_sync_us = esp_timer_get_time();
    s_answered_since_start = true;
    s_any_reachable = true; // until the task's next poll says otherwise
    taskEXIT_CRITICAL(&s_lock);
    notify(BIT_ANSWERED);
}

// Task: stop the client, and start it again on `want` when there is anything to ask.
static void restart(const timesync_servers_t *want)
{
    esp_netif_sntp_deinit();
    taskENTER_CRITICAL(&s_lock);
    s_servers = *want;
    s_start_us = esp_timer_get_time();
    s_answered_since_start = false;
    s_any_reachable = false;
    s_answered = -1;
    taskEXIT_CRITICAL(&s_lock);
    s_retry = false;
    if (want->count == 0) {
        ESP_LOGI(TAG, "off");
        return;
    }
    // lwIP keeps a slot's name and resolved address across a stop and a start: clear the slots not used now, or it
    // keeps rotating to a server of the previous list.
    for (uint8_t i = want->count; i < TIMESYNC_MAX_SERVERS; i++) {
        esp_sntp_setserver(i, NULL);
    }
    // s_servers is written only here, by this task, so its names stay put while lwIP holds pointers to them.
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(
                                   want->count, ESP_SNTP_SERVER_LIST(s_servers.addr[0], s_servers.addr[1]));
    config.wait_for_sync = false;
    config.sync_cb = on_sync;
    const esp_err_t err = esp_netif_sntp_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_sntp_init: %s, retried in %d s", esp_err_to_name(err), UPKEEP_MS / 1000);
        s_retry = true;
        return;
    }
    ESP_LOGI(TAG, "asking %s%s%s (%s)", s_servers.addr[0], want->count > 1 ? ", " : "",
             want->count > 1 ? s_servers.addr[1] : "", timesync_source_name(want->source));
}

// Task: which servers still answer, and which answered last.
static void poll_reachability(void)
{
    bool any = false;
    int8_t answered = -1;

    for (uint8_t i = 0; i < s_servers.count; i++) {
        const uint8_t reach = esp_sntp_getreachability(i);

        any = any || reach != 0;
        if (answered < 0 && (reach & 1)) {
            answered = (int8_t)i;
        }
    }
    taskENTER_CRITICAL(&s_lock);
    s_any_reachable = any;
    if (answered >= 0) {
        s_answered = answered;
    }
    taskEXIT_CRITICAL(&s_lock);
}

static void timesync_task(void *arg)
{
    for (;;) {
        uint32_t bits = 0;

        xTaskNotifyWait(0, UINT32_MAX, &bits, pdMS_TO_TICKS(UPKEEP_MS));
        if (bits & (BIT_CONFIG | BIT_DHCP)) {
            timesync_config_t cfg;
            timesync_dhcp_t dhcp;
            timesync_servers_t want, have;

            taskENTER_CRITICAL(&s_lock);
            cfg = s_cfg;
            dhcp = s_dhcp;
            have = s_servers;
            taskEXIT_CRITICAL(&s_lock);
            timesync_rules_choose(&cfg, &dhcp, &want);
            // A renewal naming the same servers changes nothing (no restart, the state stays synced).
            if (!timesync_rules_servers_equal(&want, &have) || (bits & BIT_CONFIG)) {
                restart(&want);
            }
        } else if (s_retry) {
            const timesync_servers_t again = s_servers;

            restart(&again);
        }
        if (s_servers.count > 0) {
            if (bits & BIT_ANSWERED) {
                // lwIP calls the sync callback (sntp_process) before it sets the answering server's reachability bit;
                // this task runs on the other core and would read the register in between (seen on the board: 10 s of
                // "unreachable" right after a sync). Let lwIP's thread finish the receive first.
                vTaskDelay(pdMS_TO_TICKS(ANSWER_SETTLE_MS));
            }
            poll_reachability();
        }
    }
}

/* ---- settings ---- */

static void load(timesync_config_t *out)
{
    nvs_handle_t nvs;
    uint8_t value;

    *out = (timesync_config_t) {
        .enabled = true, .mode = TIMESYNC_MODE_AUTO
    };
    const esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "nvs_open: %s, the defaults are used", esp_err_to_name(err));
        }
        return;
    }
    if (nvs_get_u8(nvs, KEY_ENABLED, &value) == ESP_OK) {
        out->enabled = value != 0;
    }
    if (nvs_get_u8(nvs, KEY_MODE, &value) == ESP_OK && value == TIMESYNC_MODE_CUSTOM) {
        out->mode = TIMESYNC_MODE_CUSTOM;
    }
    size_t len = sizeof(out->server);
    if (nvs_get_str(nvs, KEY_SERVER, out->server, &len) != ESP_OK) {
        out->server[0] = '\0';
    }
    nvs_close(nvs);
    const char *field, *reason;
    if (!timesync_rules_check(out, &field, &reason)) {
        ESP_LOGW(TAG, "stored %s refused (%s): the defaults are used", field, reason);
        *out = (timesync_config_t) {
            .enabled = true, .mode = TIMESYNC_MODE_AUTO
        };
    }
}

static esp_err_t store(const timesync_config_t *in)
{
    nvs_handle_t nvs;

    ESP_RETURN_ON_ERROR(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs), TAG, "nvs_open");
    esp_err_t err = nvs_set_u8(nvs, KEY_ENABLED, in->enabled);
    err = err == ESP_OK ? nvs_set_u8(nvs, KEY_MODE, (uint8_t)in->mode) : err;
    err = err == ESP_OK ? nvs_set_str(nvs, KEY_SERVER, in->server) : err;
    err = err == ESP_OK ? nvs_commit(nvs) : err;
    nvs_close(nvs);
    return err;
}

/* ---- public ---- */

esp_err_t timesync_init(void)
{
    ESP_RETURN_ON_FALSE(s_task == NULL, ESP_ERR_INVALID_STATE, TAG, "already started");
    timesync_config_t cfg;

    load(&cfg);
    taskENTER_CRITICAL(&s_lock);
    s_cfg = cfg;
    taskEXIT_CRITICAL(&s_lock);
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_LOST_IP, on_lost_ip, NULL), TAG,
                        "IP event handler failed");
    if (xTaskCreatePinnedToCore(timesync_task, "timesync", TASK_STACK, NULL, TASK_PRIORITY, &s_task, MGMT_CORE) !=
            pdPASS) {
        esp_event_handler_unregister(IP_EVENT, IP_EVENT_ETH_LOST_IP, on_lost_ip);
        return ESP_ERR_NO_MEM;
    }
    notify(BIT_CONFIG);
    return ESP_OK;
}

bool timesync_clock_valid(void)
{
    return timesync_rules_clock_valid((int64_t)time(NULL));
}

int64_t timesync_epoch_at(int64_t uptime_us)
{
    return timesync_rules_epoch_at((int64_t)time(NULL), esp_timer_get_time(), uptime_us);
}

void timesync_get_config(timesync_config_t *out)
{
    taskENTER_CRITICAL(&s_lock);
    *out = s_cfg;
    taskEXIT_CRITICAL(&s_lock);
}

esp_err_t timesync_set_config(const timesync_config_t *in, const char **field, const char **reason)
{
    timesync_config_t cfg = *in;

    if (!timesync_rules_check(&cfg, field, reason)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (cfg.mode == TIMESYNC_MODE_AUTO && cfg.server[0] == '\0') {
        timesync_get_config(&cfg);
        cfg.enabled = in->enabled;
        cfg.mode = TIMESYNC_MODE_AUTO; // keep the custom server for a later switch back
    }
    ESP_RETURN_ON_ERROR(store(&cfg), TAG, "storing the settings failed");
    taskENTER_CRITICAL(&s_lock);
    s_cfg = cfg;
    taskEXIT_CRITICAL(&s_lock);
    notify(BIT_CONFIG);
    return ESP_OK;
}

void timesync_get_status(timesync_status_t *out)
{
    int64_t last_sync_us, start_us;
    bool answered;

    memset(out, 0, sizeof(*out));
    taskENTER_CRITICAL(&s_lock);
    out->config = s_cfg;
    out->dhcp = s_dhcp;
    out->servers = s_servers;
    out->answered = s_answered;
    last_sync_us = s_last_sync_us;
    start_us = s_start_us;
    answered = s_answered_since_start;
    const bool reachable = s_any_reachable;
    taskEXIT_CRITICAL(&s_lock);
    const int64_t now_us = esp_timer_get_time();
    const int64_t now = (int64_t)time(NULL);
    out->now = timesync_rules_clock_valid(now) ? now : 0;
    out->synced_age_s = timesync_rules_age_s(now_us, last_sync_us);
    out->state = timesync_rules_state(out->config.enabled, now_us - start_us, answered, reachable,
                                      last_sync_us < 0 ? -1 : now_us - last_sync_us);
}
