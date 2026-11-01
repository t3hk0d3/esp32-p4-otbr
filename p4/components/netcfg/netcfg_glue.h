#pragma once

// Internal interfaces between the target-only files of netcfg (not part of the public API).

#include <stdbool.h>
#include "sdkconfig.h"
#include "esp_http_server.h"
#include "esp_eth_driver.h"
#include "esp_netif.h"
#include "netcfg_policy.h"
#include "netcfg_vlan.h"

#define NETCFG_IFACE_NAME_MAX 12
#define NETCFG_IP6_MAX CONFIG_LWIP_IPV6_NUM_ADDRESSES

/* ---- vlan_glue.c: driver-level 802.1Q ---- */

/** esp_netif driver glue of a tagged interface; esp_netif_attach() takes it as the io driver. */
typedef struct {
    esp_netif_driver_base_t base;
    esp_eth_handle_t eth;
    uint16_t vid;
} netcfg_vlan_glue_t;

void netcfg_vlan_glue_init(netcfg_vlan_glue_t *glue, esp_eth_handle_t eth, uint16_t vid);
/** Take over the Ethernet input path after all netifs are attached (the stock glue installs its own when it attaches). */
esp_err_t netcfg_vlan_install_demux(esp_eth_handle_t eth, const netcfg_demux_t *table, esp_netif_t *const netifs[NETCFG_MAX_IFACES]);
/** Frames dropped by the demux (unknown VLAN ID, no untagged interface, malformed). */
uint32_t netcfg_vlan_dropped(void);

/* ---- ifaces.c ---- */

typedef struct {
    char name[NETCFG_IFACE_NAME_MAX];
    uint16_t vlan;
    bool link;
    char ipv4[16];
    char ipv6[NETCFG_IP6_MAX][40];
    size_t ipv6_count;
} netcfg_iface_state_t;

esp_err_t netcfg_ifaces_create(esp_eth_handle_t eth, const netcfg_config_t *cfg);
esp_err_t netcfg_ifaces_start(void);
/** Undo netcfg_ifaces_create/start, also after a failure half-way: a new config can be brought up afterwards. */
void netcfg_ifaces_destroy(void);
size_t netcfg_ifaces_state(netcfg_iface_state_t out[NETCFG_MAX_IFACES]);
/** Side of the request by its local address; false when it matches no interface. */
bool netcfg_ifaces_request_side(httpd_req_t *req, netcfg_side_t *side);
/** Running mode (SINGLE before netcfg_ifaces_create). */
netcfg_mode_t netcfg_ifaces_mode(void);

/* ---- ip6_hooks.c ---- */

/** Tell the hooks which netif is mgmt (split mode) or NULL (no isolation to apply). */
void netcfg_ip6_hooks_configure(esp_netif_t *mgmt, esp_netif_t *backbone);
