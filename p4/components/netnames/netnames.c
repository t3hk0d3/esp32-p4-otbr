// Network names: storage in NVS and the glue that applies them to the mDNS library, ESP-IDF's meshcop publisher and the
// DHCP client of the netifs. The decisions (validation, defaults, which steps a save needs) are in rules.c.
#include "netnames.h"

#include <stdio.h>
#include <string.h>

#include "br_core.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mdns.h"
#include "netcfg.h"
#include "netnames_rules.h"
#include "nvs.h"

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

_Static_assert(NETNAMES_LIVE_LEN >= MDNS_NAME_BUF_LEN, "mdns_hostname_get writes up to MDNS_NAME_BUF_LEN bytes");

static struct {
    SemaphoreHandle_t lock; /* guards everything below */
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

/* The hostname the mDNS library uses now (a conflict renames it), read from the library itself: "" when mDNS does not run. */
static void live_get(char out[NETNAMES_LIVE_LEN])
{
    if (mdns_hostname_get(out) != ESP_OK) {
        out[0] = '\0';
    }
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

/* The HA name is the meshcop instance (br_core sets it safely against the OpenThread task) and the default instance. */
static esp_err_t apply_ha_name(void)
{
    const esp_err_t instance = mdns_instance_name_set(s.ha_name);
    const esp_err_t meshcop = br_core_set_meshcop_name(s.ha_name);

    return meshcop != ESP_OK ? meshcop : instance;
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
    br_core_set_meshcop_name(s.ha_name); // before OpenThread starts: kept for its first publish
    xSemaphoreGive(s.lock);
    ESP_LOGI(TAG, "hostname %s, HA name \"%s\", DHCP name %s, web UI advert %s", s.hostname, s.ha_name, s.dhcp_name,
             s.set.http_advert ? "on" : "off");
    return ESP_OK;
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
    live_get(out->hostname_live);
    xSemaphoreGive(s.lock);
}

esp_err_t netnames_set(const netnames_settings_t *in, netnames_result_t *res)
{
    netnames_settings_t next = *in;
    char live[NETNAMES_LIVE_LEN];

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
