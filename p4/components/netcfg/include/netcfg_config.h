#pragma once

// Network configuration of the board (spec 5.13): types, defaults, validation and the JSON form used by
// /device/network. Pure code, host-tested in test_host/.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_netif_ip_addr.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NETCFG_VLAN_UNTAGGED 0
#define NETCFG_VLAN_MIN 1
#define NETCFG_VLAN_MAX 4094
/** Longest JSON text netcfg_from_json looks at; a longer text is rejected before it is parsed. */
#define NETCFG_JSON_MAX 1024

typedef enum {
    NETCFG_MODE_SINGLE,
    NETCFG_MODE_SPLIT,
} netcfg_mode_t;

typedef struct {
    uint16_t vlan; /**< 0 = untagged */
    bool dhcp;
    esp_ip4_addr_t addr, mask, gw, dns; /**< static IPv4 (dhcp == false); gw and dns 0 = none */
} netcfg_iface_cfg_t;

typedef struct {
    netcfg_mode_t mode;
    netcfg_iface_cfg_t single, mgmt, thread_bb; /**< single is used in SINGLE mode, the other two in SPLIT mode */
    bool trel; /**< Thread Radio Encapsulation Link on the backbone interface; changes apply live, without a trial */
} netcfg_config_t;

/** Build-time defaults: single mode, untagged, DHCPv4, TREL on. */
void netcfg_defaults(netcfg_config_t *out);

/**
 * Check a configuration. ESP_OK, or ESP_ERR_INVALID_ARG with `*bad_field` (may be NULL) naming the first offending field:
 * "mode", "vlan" (out of range, or split mode with two untagged interfaces or equal IDs), "ipv4.addr", "ipv4.mask"
 * (not contiguous), "ipv4.gw", "ipv4.dns". The names are static strings. Only the interfaces the mode uses are checked.
 */
esp_err_t netcfg_validate(const netcfg_config_t *cfg, const char **bad_field);

/** True when both configurations describe the same network: the same mode, the same interfaces for that mode, the same TREL. */
bool netcfg_equal(const netcfg_config_t *a, const netcfg_config_t *b);

/** True when the configurations differ in `trel` and nothing else: such a change is applied live, without a trial. */
bool netcfg_diff_only_trel(const netcfg_config_t *a, const netcfg_config_t *b);

/**
 * Parse and validate the JSON of spec 5.13. `out` is filled with the defaults first; interfaces the mode does not use stay
 * default, and a missing "trel" means true (configs stored before TREL existed). ESP_OK, or ESP_ERR_INVALID_ARG with
 * `*bad_field` (may be NULL): the field names of netcfg_validate, plus "json" (not a JSON object, or longer than
 * NETCFG_JSON_MAX), "mode", "single" / "mgmt" / "thread_bb" (section missing or not an object), "ipv4" (missing or
 * malformed), "trel" (not a boolean).
 */
esp_err_t netcfg_from_json(const char *json, netcfg_config_t *out, const char **bad_field);

/** netcfg_from_json for a change request: a missing "trel" keeps `base->trel` instead of meaning true. */
esp_err_t netcfg_from_json_over(const char *json, const netcfg_config_t *base, netcfg_config_t *out, const char **bad_field);

/** JSON text of the configuration (only the interfaces the mode uses); the caller frees it with free(). NULL when out of memory. */
char *netcfg_to_json(const netcfg_config_t *cfg);

#ifdef __cplusplus
}
#endif
