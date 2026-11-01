#pragma once

// Network interfaces, VLANs, isolation, per-interface policy and confirm-or-revert (spec 5.13).
//
// Pure parts (host-tested): netcfg_config.h (types, validation, JSON), netcfg_policy.h (policy table), netcfg_trial.h
// (confirm-or-revert state machine), netcfg_vlan.h (802.1Q demux), netcfg_isolation.h (IPv6 hook verdicts).
// This header adds the runtime: netifs, driver glue, hooks and the NVS-backed trial.

#include "esp_err.h"
#include "esp_eth_driver.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "netcfg_config.h"
#include "netcfg_policy.h"
#include "netcfg_trial.h"
#include "netcfg_vlan.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Bring up the interfaces of the active (or trial) configuration on the installed, not yet started Ethernet driver:
 * loads the config from NVS namespace "otbr_net" (keys "active", "trial", "trial_boots"), creates the netif(s), installs
 * the VLAN demux and the IPv6 hooks, registers the netifs with mDNS, starts the driver and, on a trial boot, the 5-minute
 * confirm timer. Must run on core 0 (NET_CORE), after esp_netif_init(), the default event loop and nvs_flash_init(), and
 * once. Without any stored config the defaults apply (single, untagged, DHCPv4: the boot behaves as before netcfg).
 *
 * The HTTP server may already run: it listens on any address and decides per request (netcfg_request_allowed), so
 * interfaces that appear later are served as soon as they have an address. Until this call every request is allowed
 * (no interface exists, so none arrives).
 *
 * Boot never depends on a VLAN switch: with the default config no VLAN is used; a tagged config just sees no frames until
 * a trunk is there.
 */
esp_err_t netcfg_start(esp_eth_handle_t eth);

/** The interface the OpenThread backbone uses (thread_bb; the only netif in single mode). NULL before netcfg_start. */
esp_netif_t *netcfg_backbone_netif(void);
/** The management interface (mgmt; the only netif in single mode). NULL before netcfg_start. */
esp_netif_t *netcfg_mgmt_netif(void);

/**
 * Is the request allowed to use `svc` on the interface it arrived on (spec 5.13 rule 3; the interface is found from the
 * local address of the request's socket)? Single mode allows everything without looking. If not allowed: sends 403
 * `{"error":"forbidden","detail":...}` with Connection: close and returns false; the caller returns ESP_FAIL so that the
 * server closes the connection. In split mode a request whose local address matches no interface is refused.
 */
bool netcfg_request_allowed(httpd_req_t *req, netcfg_service_t svc);

/**
 * Validate `cfg` and store it as the pending trial, then reboot (after a short delay, so the reply can be sent) into it.
 * ESP_ERR_INVALID_ARG for an invalid config (nothing stored; netcfg_validate names the field).
 */
esp_err_t netcfg_set_trial(const netcfg_config_t *cfg);

/**
 * Commit the running trial: it becomes the active config. ESP_ERR_INVALID_STATE when no trial runs, ESP_ERR_NOT_ALLOWED when
 * the request did not arrive on the management side of the trial (the trial stays pending).
 */
esp_err_t netcfg_confirm(httpd_req_t *req);

/** Erase the stored configs and reboot: the next boot uses the defaults. */
esp_err_t netcfg_reset(void);

/**
 * JSON for GET /device/network: the running mode, the active config, the trial (config plus remaining_s) or null, the number of
 * received frames the VLAN demux dropped (vlan_rx_dropped), `fallback` (true, with `error`, when the stored config could not
 * be applied and the defaults run instead) and per interface name, VLAN ID (0 untagged), link, IPv4 and IPv6
 * addresses. Caller frees with free(). NULL when out of memory.
 */
char *netcfg_state_json(void);

/** Register the console command `net` (`net show`, `net reset`); the console must be initialised. */
esp_err_t netcfg_console_register(void);

#ifdef __cplusplus
}
#endif
