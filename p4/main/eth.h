#pragma once

#include "esp_err.h"
#include "esp_eth_driver.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Install the on-board Ethernet driver (IP101 PHY on the internal EMAC) and the logging of link and address events. The
 * driver is not started and has no netif: netcfg_start() creates the interfaces from the network config and starts it
 * (starting earlier would lose the link-up events the interfaces need).
 *
 * Must be called from a task pinned to core 0 (the EMAC interrupt is allocated on the calling core), after
 * esp_netif_init() and the default event loop exist. Logs "eth: got ip <addr>" for each IPv4 address obtained and
 * "eth: got ip6 <addr> link-local|slaac" for IPv6 addresses.
 *
 * @param out_eth receives the driver handle; must not be NULL
 */
esp_err_t eth_driver_start(esp_eth_handle_t *out_eth);

#ifdef __cplusplus
}
#endif
