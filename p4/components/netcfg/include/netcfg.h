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

struct sockaddr;

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
 * Same decision for a connected non-HTTP socket (the RCP proxy's TCP listen clients): may the connection on `fd` use `svc` on the
 * interface its local address belongs to? A local address on the OpenThread netif (the mesh) is refused in every mode,
 * single included. Otherwise single mode allows everything (also when the interface is not found); split mode applies
 * the policy and refuses a connection whose interface is not found. Takes the netcfg lock for at most 100 ms to look the
 * interface up; if it cannot, logs a warning and refuses. Sends nothing on the socket: the caller closes it. Call it
 * from a task that does not hold the netcfg lock.
 */
bool netcfg_socket_allowed(int fd, netcfg_service_t svc);

/**
 * The same decision for a peer address (the RCP proxy's UDP sources, TCP listen clients and connect targets): may
 * `peer` (sockaddr_in, or sockaddr_in6 with an IPv4-mapped address on a dual-stack socket) use `svc`? Its interface is
 * the one a reply to it leaves on (lwIP's route lookup, a short call into the tcpip task; a link-local address goes by
 * its zone). A peer routed to the OpenThread netif is refused in every mode; otherwise as netcfg_socket_allowed
 * (netcfg_policy_conn_allowed). Call it from a task that does not hold the netcfg lock, never from the tcpip task.
 */
bool netcfg_peer_allowed(const struct sockaddr *peer, netcfg_service_t svc);

/** netcfg_peer_allowed with `noun` naming the peer in the refusal log ("a client", "a target"; the plain one says "a source"). */
bool netcfg_peer_allowed_as(const struct sockaddr *peer, netcfg_service_t svc, const char *noun);

/**
 * netcfg_peer_allowed without its log: a refusal's reason ("on a mesh address", "its interface is unknown", ...; a
 * string literal) goes to *why (may be NULL; set to NULL when allowed), for a caller that rate-limits its own log (the
 * RCP proxy's UDP sources).
 */
bool netcfg_peer_check(const struct sockaddr *peer, netcfg_service_t svc, const char **why);

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

/** Copy of the configuration this boot runs (the trial during a trial, the defaults after a fallback). ESP_ERR_INVALID_STATE before netcfg_start. */
esp_err_t netcfg_get_running(netcfg_config_t *out);

/**
 * Store the TREL setting in the active config without a trial or a reboot (netcfg_trial_set_trel) and make it the running
 * value; the caller applies it to OpenThread (br_core_set_trel). ESP_ERR_INVALID_STATE before netcfg_start or while a trial
 * runs. No flash write when the value does not change.
 */
esp_err_t netcfg_set_trel(bool enabled);

/** TREL setting of the running configuration; true (the default) before netcfg_start. */
bool netcfg_trel_enabled(void);

/** Erase the stored configs and reboot: the next boot uses the defaults. */
esp_err_t netcfg_reset(void);

/**
 * DHCP client name (option 12) of every interface: taken by the interfaces netcfg_start makes, and applied at once to the
 * running ones, whose DHCP client renews its lease to send it without giving up the address. Empty = lwIP's default.
 * Callable before netcfg_start. ESP_ERR_INVALID_ARG when NULL or longer than 32 characters.
 */
esp_err_t netcfg_set_dhcp_hostname(const char *name);

/** The running mode; NETCFG_MODE_SINGLE before netcfg_start. */
netcfg_mode_t netcfg_running_mode(void);

/** A trial configuration waits for confirmation. false before netcfg_start. */
bool netcfg_trial_pending(void);

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
