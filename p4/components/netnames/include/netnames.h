#pragma once

// Names of the board on the network (spec docs/superpowers/specs/2026-11-03-network-names-design.md): the mDNS hostname,
// the name Home Assistant shows (the _meshcop._udp instance), the optional _http._tcp advertisement of the web UI and the
// DHCP client name. Stored in NVS, applied at boot and live.

#include <stdbool.h>
#include "esp_err.h"
#include "netcfg_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Buffer size of every name: up to 63 characters (bytes of UTF-8 for the HA name) plus the terminator. */
#define NETNAMES_NAME_LEN 64
/** Buffer of the hostname the mDNS library uses: its MDNS_NAME_BUF_LEN (a conflict rename can add "-2" to 63 chars). */
#define NETNAMES_LIVE_LEN 65
/** Default DHCP client name (replaces lwIP's CONFIG_LWIP_LOCAL_HOSTNAME). */
#define NETNAMES_DEFAULT_DHCP_NAME "esp-otbr"

/** What the user sets. Empty hostname / DHCP name = the default; ha_name is used only when it does not follow. */
typedef struct {
    char hostname[NETNAMES_NAME_LEN];
    bool ha_name_follows_hostname;
    char ha_name[NETNAMES_NAME_LEN];
    bool http_advert;
    char dhcp_name[NETNAMES_NAME_LEN];
} netnames_settings_t;

/** The settings plus the names in effect (defaults resolved). */
typedef struct {
    netnames_settings_t set;
    char hostname[NETNAMES_NAME_LEN];         /**< effective hostname */
    char hostname_default[NETNAMES_NAME_LEN]; /**< esp-otbr-<last 3 MAC bytes> */
    char hostname_live[NETNAMES_LIVE_LEN];    /**< what the mDNS library uses; differs after a conflict rename, "" without mDNS */
    char ha_name[NETNAMES_NAME_LEN];          /**< effective Home Assistant (meshcop instance) name */
    char dhcp_name[NETNAMES_NAME_LEN];        /**< effective DHCP client name */
    bool http_advert_available;               /**< false in split mode: the web UI is not on the mDNS interface */
} netnames_t;

/** Outcome of netnames_set. */
typedef struct {
    const char *field;  /**< invalid field ("hostname", "ha_name", "dhcp_name") on ESP_ERR_INVALID_ARG, else NULL */
    const char *reason; /**< why it is invalid */
    bool applied;       /**< every live step worked; when false the stored values apply at the next boot */
    char detail[96];    /**< the first step that failed, "" when applied */
} netnames_result_t;

/**
 * Load the stored names (missing = defaults; unreadable storage is logged and the defaults are used) and hand the HA name
 * to ESP-IDF's meshcop publisher. Call after storage_init and before br_core_mdns_start.
 */
esp_err_t netnames_init(void);

/** The running network mode, once netcfg has started: decides whether the _http._tcp advert can be published. */
void netnames_set_mode(netcfg_mode_t mode);

void netnames_get(netnames_t *out);

/**
 * Validate, store and apply live. ESP_ERR_INVALID_ARG with res->field / res->reason (nothing stored), ESP_FAIL when
 * storing failed (nothing applied), else ESP_OK; res->applied is false and res->detail says which step failed when a live
 * step did not work (the stored value applies at the next boot).
 */
esp_err_t netnames_set(const netnames_settings_t *in, netnames_result_t *res);

/** Effective names for the boot path; valid after netnames_init and for the board's lifetime. */
const char *netnames_hostname(void);
const char *netnames_ha_name(void);
const char *netnames_dhcp_name(void);

#ifdef __cplusplus
}
#endif
