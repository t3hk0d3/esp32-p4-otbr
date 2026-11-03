// Network names: storage in NVS and the glue that applies them to the mDNS library, ESP-IDF's meshcop publisher and the
// DHCP client of the netifs. The decisions (validation, defaults, which steps a save needs) are in rules.c.
#include "netnames.h"

#include <stdio.h>
#include <string.h>

#include "br_core.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_openthread.h"
#include "esp_openthread_border_router.h"
#include "esp_openthread_lock.h"
#include "esp_openthread_meshcop_mdns.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mdns.h"
#include "netcfg.h"
#include "netnames_rules.h"
#include "nvs.h"
#include "openthread/thread.h"

static const char *TAG = "netnames";

#define NVS_NAMESPACE "otbr_names"
#define KEY_HOSTNAME "host"
#define KEY_HA_FOLLOWS "ha_follow"
#define KEY_HA_NAME "ha"
#define KEY_HTTP "http"
#define KEY_DHCP "dhcp"
#define HTTP_SERVICE "_http"
#define HTTP_PROTO "_tcp"
#define HTTP_PORT 80

static struct {
    SemaphoreHandle_t lock; /* guards everything below except `live` */
    StaticSemaphore_t lock_storage;
    netnames_settings_t set;
    char hostname_default[NETNAMES_NAME_LEN];
    char hostname[NETNAMES_NAME_LEN]; /* effective names: live for the board's lifetime, the boot path and OpenThread read them */
    char ha_name[NETNAMES_NAME_LEN];
    char dhcp_name[NETNAMES_NAME_LEN];
    netcfg_mode_t mode;
    bool mode_known;    /* netnames_set_mode was called: the _http._tcp advert can be decided */
    bool http_published;
} s = {.set = {.ha_name_follows_hostname = true}};

/* The hostname the mDNS library uses (a conflict renames it); written by the library's callback on the mDNS task. */
static char s_live[NETNAMES_NAME_LEN];
static portMUX_TYPE s_live_lock = portMUX_INITIALIZER_UNLOCKED;

/* ---- live hostname ---- */

static void live_set(const char *hostname)
{
    taskENTER_CRITICAL(&s_live_lock);
    strlcpy(s_live, hostname ? hostname : "", sizeof(s_live));
    taskEXIT_CRITICAL(&s_live_lock);
}

static void live_get(char out[NETNAMES_NAME_LEN])
{
    taskENTER_CRITICAL(&s_live_lock);
    strlcpy(out, s_live, NETNAMES_NAME_LEN);
    taskEXIT_CRITICAL(&s_live_lock);
}

static void on_hostname_changed(const char *hostname, void *arg)
{
    live_set(hostname);
}

/* ---- storage ---- */

static void load_str(nvs_handle_t nvs, const char *key, char out[NETNAMES_NAME_LEN])
{
    size_t len = NETNAMES_NAME_LEN;

    if (nvs_get_str(nvs, key, out, &len) != ESP_OK) {
        out[0] = '\0';
    }
}

static void load_bool(nvs_handle_t nvs, const char *key, bool *out)
{
    uint8_t value;

    if (nvs_get_u8(nvs, key, &value) == ESP_OK) {
        *out = value != 0;
    }
}

static void load(netnames_settings_t *out)
{
    nvs_handle_t nvs;
    const esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);

    if (err != ESP_OK) {
        if (err != ESP_ERR_NVS_NOT_FOUND) { // not found: nothing stored yet
            ESP_LOGE(TAG, "opening the stored names failed (%s): defaults", esp_err_to_name(err));
        }
        return;
    }
    load_str(nvs, KEY_HOSTNAME, out->hostname);
    load_bool(nvs, KEY_HA_FOLLOWS, &out->ha_name_follows_hostname);
    load_str(nvs, KEY_HA_NAME, out->ha_name);
    load_bool(nvs, KEY_HTTP, &out->http_advert);
    load_str(nvs, KEY_DHCP, out->dhcp_name);
    nvs_close(nvs);
    const char *field = NULL;
    const char *reason = NULL;

    if (!netnames_rules_check(out, &field, &reason)) { // never written by this firmware: defaults rather than a broken name
        ESP_LOGE(TAG, "stored %s %s: defaults", field, reason);
        *out = (netnames_settings_t) {
            .ha_name_follows_hostname = true
        };
    }
}

static esp_err_t store(const netnames_settings_t *in)
{
    nvs_handle_t nvs;

    ESP_RETURN_ON_ERROR(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs), TAG, "nvs_open");
    esp_err_t err = nvs_set_str(nvs, KEY_HOSTNAME, in->hostname);

    err = err == ESP_OK ? nvs_set_u8(nvs, KEY_HA_FOLLOWS, in->ha_name_follows_hostname) : err;
    err = err == ESP_OK ? nvs_set_str(nvs, KEY_HA_NAME, in->ha_name) : err;
    err = err == ESP_OK ? nvs_set_u8(nvs, KEY_HTTP, in->http_advert) : err;
    err = err == ESP_OK ? nvs_set_str(nvs, KEY_DHCP, in->dhcp_name) : err;
    err = err == ESP_OK ? nvs_commit(nvs) : err;
    nvs_close(nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "storing the names failed: %s", esp_err_to_name(err));
    }
    return err;
}

/* ---- apply ---- */

static void resolve(void)
{
    netnames_rules_effective(&s.set, s.hostname_default, s.hostname, s.ha_name, s.dhcp_name);
}

static bool http_wanted(void)
{
    return s.set.http_advert && s.mode_known && s.mode == NETCFG_MODE_SINGLE;
}

/* Publish, remove or rename the _http._tcp service to match the settings and the mode. */
static esp_err_t apply_http(void)
{
    static mdns_txt_item_t txt[] = {{"path", "/"}};
    esp_err_t err = ESP_OK;

    if (http_wanted() && !s.http_published) {
        err = mdns_service_add(s.ha_name, HTTP_SERVICE, HTTP_PROTO, HTTP_PORT, txt, sizeof(txt) / sizeof(txt[0]));
        s.http_published = err == ESP_OK;
    } else if (http_wanted()) {
        err = mdns_service_instance_name_set(HTTP_SERVICE, HTTP_PROTO, s.ha_name);
    } else if (s.http_published) {
        err = mdns_service_remove(HTTP_SERVICE, HTTP_PROTO);
        s.http_published = err != ESP_OK;
    }
    return err;
}

/* The HA name is the meshcop instance: stored for the next role change, and republished now when the service is up. */
static esp_err_t apply_ha_name(void)
{
    esp_err_t err = mdns_instance_name_set(s.ha_name);
    const br_core_mode_t mode = br_core_get_mode();

    // The setter frees the previous name, which the role-change handler reads on the OpenThread task: under the lock while
    // OpenThread runs. STARTING and RADIO_FAULT have no running OpenThread task (and maybe no lock yet); the next start
    // publishes with the name stored here.
    // The session keeps a radio push from deleting the lock under this call; none opens once a push has begun. During a
    // push OpenThread may still run (the push waits for open sessions first), so the name is not set at all: it is
    // stored, and the next boot publishes it.
    if ((mode != BR_MODE_BR_RUNNING && mode != BR_MODE_SNIFFER) || !br_core_ot_session_begin()) {
        if (br_core_radio_job_running()) {
            return err == ESP_OK ? ESP_ERR_INVALID_STATE : err;
        }
        esp_openthread_set_meshcop_instance_name(s.ha_name);
        return err;
    }
    esp_openthread_lock_acquire(portMAX_DELAY);
    esp_openthread_set_meshcop_instance_name(s.ha_name);
    const otDeviceRole role = otThreadGetDeviceRole(esp_openthread_get_instance());

    if (mode == BR_MODE_BR_RUNNING &&
            (role == OT_DEVICE_ROLE_CHILD || role == OT_DEVICE_ROLE_ROUTER || role == OT_DEVICE_ROLE_LEADER)) {
        // publish returns early while the service is published, so a rename is a remove first.
        esp_openthread_remove_meshcop_mdns();
        const esp_err_t published = esp_openthread_publish_meshcop_mdns(s.ha_name);

        err = err == ESP_OK ? published : err;
    }
    esp_openthread_lock_release();
    br_core_ot_session_end();
    return err;
}

static void note_step(netnames_result_t *res, const char *step, esp_err_t err)
{
    if (err == ESP_OK) {
        return;
    }
    ESP_LOGW(TAG, "%s: %s", step, esp_err_to_name(err));
    if (res && res->applied) {
        res->applied = false;
        snprintf(res->detail, sizeof(res->detail), "%s not applied: %s; it applies at the next boot", step,
                 esp_err_to_name(err));
    }
}

/* ---- API ---- */

esp_err_t netnames_init(void)
{
    uint8_t mac[6] = {0};

    if (!s.lock) {
        s.lock = xSemaphoreCreateMutexStatic(&s.lock_storage);
    }
    ESP_RETURN_ON_ERROR(esp_read_mac(mac, ESP_MAC_ETH), TAG, "reading the MAC");
    xSemaphoreTake(s.lock, portMAX_DELAY);
    netnames_rules_default_hostname(mac, s.hostname_default);
    load(&s.set);
    resolve();
    live_set(s.hostname);
    // No init-state guard in the prebuilt library: it keeps a copy, the role-change handler publishes with it.
    esp_openthread_set_meshcop_instance_name(s.ha_name);
    xSemaphoreGive(s.lock);
    ESP_LOGI(TAG, "hostname %s, HA name \"%s\", DHCP name %s, web UI advert %s", s.hostname, s.ha_name, s.dhcp_name,
             s.set.http_advert ? "on" : "off");
    return ESP_OK;
}

esp_err_t netnames_mdns_started(void)
{
    char live[NETNAMES_NAME_LEN];

    if (mdns_hostname_get(live) == ESP_OK) {
        live_set(live);
    }
    return mdns_register_hostname_changed_callback(on_hostname_changed, NULL);
}

void netnames_set_mode(netcfg_mode_t mode)
{
    xSemaphoreTake(s.lock, portMAX_DELAY);
    s.mode = mode;
    s.mode_known = true;
    note_step(NULL, "web UI advert", apply_http());
    xSemaphoreGive(s.lock);
}

void netnames_get(netnames_t *out)
{
    xSemaphoreTake(s.lock, portMAX_DELAY);
    out->set = s.set;
    strlcpy(out->hostname, s.hostname, sizeof(out->hostname));
    strlcpy(out->hostname_default, s.hostname_default, sizeof(out->hostname_default));
    strlcpy(out->ha_name, s.ha_name, sizeof(out->ha_name));
    strlcpy(out->dhcp_name, s.dhcp_name, sizeof(out->dhcp_name));
    out->http_advert_available = !s.mode_known || s.mode == NETCFG_MODE_SINGLE;
    xSemaphoreGive(s.lock);
    live_get(out->hostname_live);
}

esp_err_t netnames_set(const netnames_settings_t *in, netnames_result_t *res)
{
    netnames_settings_t next = *in;
    char live[NETNAMES_NAME_LEN];

    *res = (netnames_result_t) {
        .applied = true
    };
    if (!netnames_rules_check(&next, &res->field, &res->reason)) {
        return ESP_ERR_INVALID_ARG;
    }
    netnames_rules_normalise(&next);
    xSemaphoreTake(s.lock, portMAX_DELAY);
    live_get(live);
    const netnames_steps_t steps = netnames_rules_steps(&s.set, &next, s.hostname_default, live);
    const esp_err_t stored = store(&next);

    if (stored != ESP_OK) {
        xSemaphoreGive(s.lock);
        return ESP_FAIL;
    }
    s.set = next;
    resolve();
    if (steps.hostname) {
        note_step(res, "hostname", mdns_hostname_set(s.hostname));
    }
    if (steps.ha_name) {
        note_step(res, "Home Assistant name", apply_ha_name());
    }
    if (steps.http_advert) {
        note_step(res, "web UI advert", apply_http());
    }
    if (steps.dhcp_name) {
        note_step(res, "DHCP name", netcfg_set_dhcp_hostname(s.dhcp_name));
    }
    xSemaphoreGive(s.lock);
    ESP_LOGI(TAG, "saved: hostname %s, HA name \"%s\", DHCP name %s, web UI advert %s", s.hostname, s.ha_name, s.dhcp_name,
             next.http_advert ? "on" : "off");
    return ESP_OK;
}

const char *netnames_hostname(void)
{
    return s.hostname;
}

const char *netnames_ha_name(void)
{
    return s.ha_name;
}

const char *netnames_dhcp_name(void)
{
    return s.dhcp_name;
}
