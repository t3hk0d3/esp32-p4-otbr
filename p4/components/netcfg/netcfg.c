// netcfg lifecycle: loads the config from NVS, brings up the interfaces, runs the confirm timer and serves the runtime API
// (spec 5.13). Decisions live in the pure files (trial.c, policy.c, config.c); this file is storage, timers and locks.
#include "netcfg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board_cores.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "netcfg_glue.h"
#include "netcfg_priv.h"
#include "nvs.h"
#include "reboot_gate.h"

static const char *TAG = "netcfg";

#define NVS_NAMESPACE "otbr_net"
#define REBOOT_DELAY_MS 1000 /* lets the caller of a request get its reply before the reboot */
#define US_PER_MS 1000ULL
#define US_PER_S (1000ULL * US_PER_MS)
#define FORBIDDEN_BODY "{\"error\":\"forbidden\",\"detail\":\"this service is not available on this interface\"}"

static struct {
    SemaphoreHandle_t lock; /* guards trial and running; never held across network I/O */
    nvs_handle_t nvs;
    bool nvs_open;
    netcfg_trial_t trial;
    netcfg_config_t running;
    esp_timer_handle_t trial_timer;
    bool start_called;
    bool ready;         /* lock, storage and trial state exist: the API that needs only those works (net reset, PUT, confirm) */
    bool fallback;      /* the stored config could not be applied: the defaults run */
    char fallback_error[48];
} s;

/* ---- storage, clock and restart for the trial state machine ---- */

static esp_err_t nvs_read(void *ctx, const char *key, char *buf, size_t cap)
{
    size_t len = cap;

    if (!s.nvs_open) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t err = nvs_get_str(s.nvs, key, buf, &len);

    return err == ESP_ERR_NVS_NOT_FOUND ? ESP_ERR_NOT_FOUND : err;
}

static esp_err_t nvs_write(void *ctx, const char *key, const char *text)
{
    ESP_RETURN_ON_FALSE(s.nvs_open, ESP_ERR_INVALID_STATE, TAG, "no storage");
    ESP_RETURN_ON_ERROR(nvs_set_str(s.nvs, key, text), TAG, "storing %s failed", key);
    return nvs_commit(s.nvs);
}

static esp_err_t nvs_read_u8(void *ctx, const char *key, uint8_t *value)
{
    if (!s.nvs_open) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t err = nvs_get_u8(s.nvs, key, value);

    return err == ESP_ERR_NVS_NOT_FOUND ? ESP_ERR_NOT_FOUND : err;
}

static esp_err_t nvs_write_u8(void *ctx, const char *key, uint8_t value)
{
    ESP_RETURN_ON_FALSE(s.nvs_open, ESP_ERR_INVALID_STATE, TAG, "no storage");
    ESP_RETURN_ON_ERROR(nvs_set_u8(s.nvs, key, value), TAG, "storing %s failed", key);
    return nvs_commit(s.nvs);
}

static esp_err_t nvs_erase(void *ctx, const char *key)
{
    ESP_RETURN_ON_FALSE(s.nvs_open, ESP_ERR_INVALID_STATE, TAG, "no storage");
    const esp_err_t err = nvs_erase_key(s.nvs, key);

    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        return err;
    }
    return nvs_commit(s.nvs);
}

static uint64_t clock_ms(void *ctx)
{
    return (uint64_t)esp_timer_get_time() / US_PER_MS;
}

// The trial expiry, a trial store and `net reset` reboot through the gate: a running update, radio push or ROM
// recovery is not cut off; the reboot follows once it ends.
static void request_restart(void *ctx)
{
    reboot_gate_restart(REBOOT_DELAY_MS);
}

static const netcfg_env_t s_env = {
    .read = nvs_read, .write = nvs_write, .read_u8 = nvs_read_u8, .write_u8 = nvs_write_u8,
    .erase = nvs_erase, .now_ms = clock_ms, .restart = request_restart,
};

/* ---- confirm timer ---- */

static void on_trial_timer(void *arg)
{
    xSemaphoreTake(s.lock, portMAX_DELAY);
    const bool expired = netcfg_trial_poll(&s.trial);

    xSemaphoreGive(s.lock);
    if (expired) {
        ESP_LOGW(TAG, "trial not confirmed in %d s: discarded, rebooting into the previous config", NETCFG_TRIAL_TIMEOUT_S);
    }
}

static esp_err_t start_trial_timer(void)
{
    const esp_timer_create_args_t args = {.callback = on_trial_timer, .name = "netcfg_trial"};

    ESP_RETURN_ON_ERROR(esp_timer_create(&args, &s.trial_timer), TAG, "creating the trial timer failed");
    return esp_timer_start_once(s.trial_timer, netcfg_trial_remaining_s(&s.trial) * US_PER_S);
}

/* ---- API ---- */

static esp_err_t bring_up(esp_eth_handle_t eth)
{
    ESP_RETURN_ON_ERROR(netcfg_ifaces_create(eth, &s.running), TAG, "creating the interfaces failed");
    return netcfg_ifaces_start();
}

static bool running_is_defaults(void)
{
    netcfg_config_t defaults;

    netcfg_defaults(&defaults);
    defaults.trel = s.running.trel; // only the interfaces decide whether a fallback can help
    return netcfg_equal(&defaults, &s.running);
}

esp_err_t netcfg_start(esp_eth_handle_t eth)
{
    ESP_RETURN_ON_FALSE(xPortGetCoreID() == NET_CORE, ESP_ERR_INVALID_STATE, TAG, "must run on core %d", NET_CORE);
    ESP_RETURN_ON_FALSE(!s.start_called, ESP_ERR_INVALID_STATE, TAG, "already started");
    s.start_called = true;
    s.lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s.lock, ESP_ERR_NO_MEM, TAG, "no memory");
    s.nvs_open = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &s.nvs) == ESP_OK;
    if (!s.nvs_open) {
        ESP_LOGE(TAG, "cannot open the %s storage: running the defaults, nothing can be saved", NVS_NAMESPACE);
    }
    netcfg_trial_boot(&s.trial, &s_env, &s.running);
    // From here on `net reset` and the other storage-only calls work, whatever happens to the interfaces below.
    s.ready = true;
    ESP_LOGI(TAG, "mode %s%s", s.running.mode == NETCFG_MODE_SPLIT ? "split" : "single", s.trial.on_trial ? " (trial)" : "");

    if (s.trial.on_trial) {
        // First, so that a trial whose interfaces cannot be brought up still reverts after the confirm time.
        const esp_err_t timer_err = start_trial_timer();

        if (timer_err != ESP_OK) {
            ESP_LOGE(TAG, "starting the trial timer failed (%s): reverting the trial", esp_err_to_name(timer_err));
            netcfg_trial_revert(&s.trial);
            return timer_err;
        }
        ESP_LOGW(TAG, "network trial: POST /device/network/confirm within %d s or the previous config returns", NETCFG_TRIAL_TIMEOUT_S);
    }
    esp_err_t err = bring_up(eth);
    netcfg_apply_action_t action = netcfg_apply_decide(err == ESP_OK, s.trial.on_trial, running_is_defaults());

    if (action == NETCFG_APPLY_TRY_DEFAULTS) {
        ESP_LOGE(TAG, "the stored network config cannot be applied (%s): falling back to the defaults", esp_err_to_name(err));
        snprintf(s.fallback_error, sizeof(s.fallback_error), "%s", esp_err_to_name(err));
        netcfg_ifaces_destroy();
        const bool trel = s.running.trel; // the stored TREL choice survives an interface fallback

        netcfg_defaults(&s.running);
        s.running.trel = trel;
        s.fallback = true;
        err = bring_up(eth);
        action = netcfg_apply_decide(err == ESP_OK, false, true);
    }
    if (action == NETCFG_APPLY_REVERT_TRIAL) {
        ESP_LOGE(TAG, "the network trial cannot be applied (%s): reverting to the previous config", esp_err_to_name(err));
        netcfg_ifaces_destroy();
        netcfg_trial_revert(&s.trial);
        return err;
    }
    if (action == NETCFG_APPLY_GIVE_UP) {
        ESP_LOGE(TAG, "no network interface could be brought up (%s): HTTP and console stay, `net reset` and a reboot are the way out",
                 esp_err_to_name(err));
        netcfg_ifaces_destroy();
        return err;
    }
    return ESP_OK;
}

bool netcfg_request_allowed(httpd_req_t *req, netcfg_service_t svc)
{
    if (netcfg_ifaces_mode() == NETCFG_MODE_SINGLE) { /* nothing to separate (also before the interfaces exist) */
        return true;
    }
    netcfg_side_t side;

    if (netcfg_ifaces_request_side(req, &side) && netcfg_policy_allows(NETCFG_MODE_SPLIT, side, svc)) {
        return true;
    }
    ESP_LOGW(TAG, "refused %s: not served on this interface", req->uri);
    httpd_resp_set_status(req, "403 Forbidden");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_send(req, FORBIDDEN_BODY, HTTPD_RESP_USE_STRLEN);
    return false;
}

esp_err_t netcfg_set_trial(const netcfg_config_t *cfg)
{
    ESP_RETURN_ON_FALSE(s.ready, ESP_ERR_INVALID_STATE, TAG, "not started");
    xSemaphoreTake(s.lock, portMAX_DELAY);
    const esp_err_t err = netcfg_trial_apply(&s_env, cfg, NULL);

    xSemaphoreGive(s.lock);
    return err;
}

esp_err_t netcfg_confirm(httpd_req_t *req)
{
    ESP_RETURN_ON_FALSE(s.ready, ESP_ERR_INVALID_STATE, TAG, "not started");
    xSemaphoreTake(s.lock, portMAX_DELAY);
    const bool on_trial = s.trial.on_trial;

    xSemaphoreGive(s.lock);
    if (!on_trial) {
        return ESP_ERR_INVALID_STATE;
    }
    netcfg_side_t side;

    if (!netcfg_ifaces_request_side(req, &side) || !netcfg_confirm_allowed(side)) {
        return ESP_ERR_NOT_ALLOWED;
    }
    xSemaphoreTake(s.lock, portMAX_DELAY);
    const esp_err_t err = netcfg_trial_confirm(&s.trial);

    xSemaphoreGive(s.lock);
    if (err == ESP_OK) {
        esp_timer_stop(s.trial_timer);
        ESP_LOGI(TAG, "trial confirmed: now the active config");
    }
    return err;
}

esp_err_t netcfg_get_running(netcfg_config_t *out)
{
    ESP_RETURN_ON_FALSE(s.ready, ESP_ERR_INVALID_STATE, TAG, "not started");
    xSemaphoreTake(s.lock, portMAX_DELAY);
    *out = s.running;
    xSemaphoreGive(s.lock);
    return ESP_OK;
}

esp_err_t netcfg_set_trel(bool enabled)
{
    ESP_RETURN_ON_FALSE(s.ready, ESP_ERR_INVALID_STATE, TAG, "not started");
    xSemaphoreTake(s.lock, portMAX_DELAY);
    const esp_err_t err = netcfg_trial_set_trel(&s.trial, enabled);

    if (err == ESP_OK) {
        s.running.trel = enabled;
    }
    xSemaphoreGive(s.lock);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "TREL %s", enabled ? "on" : "off");
    }
    return err;
}

bool netcfg_trel_enabled(void)
{
    if (!s.ready) {
        return true;
    }
    xSemaphoreTake(s.lock, portMAX_DELAY);
    const bool enabled = s.running.trel;

    xSemaphoreGive(s.lock);
    return enabled;
}

esp_err_t netcfg_reset(void)
{
    ESP_RETURN_ON_FALSE(s.ready, ESP_ERR_INVALID_STATE, TAG, "not started");
    xSemaphoreTake(s.lock, portMAX_DELAY);
    const esp_err_t err = netcfg_trial_reset(&s_env);

    xSemaphoreGive(s.lock);
    return err;
}

/* ---- state JSON ---- */

static cJSON *interface_json(const netcfg_iface_state_t *iface)
{
    cJSON *obj = cJSON_CreateObject();
    cJSON *ipv6 = cJSON_CreateArray();

    if (!obj || !ipv6) {
        cJSON_Delete(obj);
        cJSON_Delete(ipv6);
        return NULL;
    }
    cJSON_AddStringToObject(obj, "name", iface->name);
    cJSON_AddNumberToObject(obj, "vlan", iface->vlan);
    cJSON_AddBoolToObject(obj, "link", iface->link);
    if (iface->ipv4[0]) {
        cJSON_AddStringToObject(obj, "ipv4", iface->ipv4);
    } else {
        cJSON_AddNullToObject(obj, "ipv4");
    }
    for (size_t i = 0; i < iface->ipv6_count; i++) {
        cJSON_AddItemToArray(ipv6, cJSON_CreateString(iface->ipv6[i]));
    }
    cJSON_AddItemToObject(obj, "ipv6", ipv6);
    return obj;
}

// The stored active config (what the next boot uses without a trial); the running one when no trial is on.
static void active_config(netcfg_config_t *out)
{
    char text[NETCFG_STORED_MAX];

    if (nvs_read(NULL, NETCFG_KEY_ACTIVE, text, sizeof(text)) != ESP_OK || netcfg_from_json(text, out, NULL) != ESP_OK) {
        netcfg_defaults(out);
    }
}

// Add `item` to `parent`; the item is freed when it cannot be added. False for a NULL item.
static bool attach(cJSON *parent, const char *name, cJSON *item)
{
    if (!item) {
        return false;
    }
    if (!cJSON_AddItemToObject(parent, name, item)) {
        cJSON_Delete(item);
        return false;
    }
    return true;
}

static cJSON *trial_json(const netcfg_config_t *cfg, uint32_t remaining_s)
{
    cJSON *item = netcfg_config_to_cjson(cfg);

    if (item && !cJSON_AddNumberToObject(item, "remaining_s", remaining_s)) {
        cJSON_Delete(item);
        return NULL;
    }
    return item;
}

esp_err_t netcfg_set_dhcp_hostname(const char *name)
{
    ESP_RETURN_ON_FALSE(name && strlen(name) <= NETCFG_DHCP_NAME_MAX, ESP_ERR_INVALID_ARG, TAG, "bad DHCP name");
    return netcfg_ifaces_set_dhcp_name(name);
}

netcfg_mode_t netcfg_running_mode(void)
{
    return netcfg_ifaces_mode();
}

bool netcfg_trial_pending(void)
{
    if (!s.ready) {
        return false;
    }
    xSemaphoreTake(s.lock, portMAX_DELAY);
    const bool pending = s.trial.on_trial;

    xSemaphoreGive(s.lock);
    return pending;
}

char *netcfg_state_json(void)
{
    netcfg_config_t running, active;
    netcfg_trial_t trial;
    netcfg_iface_state_t ifaces[NETCFG_MAX_IFACES];

    if (!s.ready) {
        return NULL;
    }
    xSemaphoreTake(s.lock, portMAX_DELAY);
    running = s.running;
    trial = s.trial;
    const bool fallback = s.fallback;
    char fallback_error[sizeof(s.fallback_error)];

    memcpy(fallback_error, s.fallback_error, sizeof(fallback_error));
    const uint32_t remaining = netcfg_trial_remaining_s(&s.trial);

    if (trial.on_trial) {
        active_config(&active);
    } else {
        active = running;
    }
    xSemaphoreGive(s.lock);
    const size_t count = netcfg_ifaces_state(ifaces);

    cJSON *root = cJSON_CreateObject();
    cJSON *list = cJSON_CreateArray();
    bool ok = root && list && cJSON_AddStringToObject(root, "mode", running.mode == NETCFG_MODE_SPLIT ? "split" : "single") &&
              attach(root, "active", netcfg_config_to_cjson(&active)) &&
              attach(root, "trial", trial.on_trial ? trial_json(&trial.trial, remaining) : cJSON_CreateNull()) &&
              cJSON_AddNumberToObject(root, "vlan_rx_dropped", netcfg_vlan_dropped()) && cJSON_AddBoolToObject(root, "fallback", fallback) &&
              (!fallback || cJSON_AddStringToObject(root, "error", fallback_error));

    const bool list_handed_over = ok; /* attach() takes the list, and frees it when it fails */

    if (ok) {
        ok = attach(root, "interfaces", list); /* root owns the list from here on, also for the items added below */
    }
    for (size_t i = 0; ok && i < count; i++) {
        cJSON *item = interface_json(&ifaces[i]);

        ok = item && cJSON_AddItemToArray(list, item);
        if (item && !ok) {
            cJSON_Delete(item);
        }
    }
    char *text = ok ? cJSON_PrintUnformatted(root) : NULL;

    cJSON_Delete(root);
    if (!list_handed_over) {
        cJSON_Delete(list);
    }
    return text;
}
