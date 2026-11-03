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

/** What the user sets. Empty hostname / DHCP name = the default; ha_name is used only when it does not follow. */
typedef struct {
    char hostname[NETNAMES_NAME_LEN];
    bool ha_name_follows_hostname;
    char ha_name[NETNAMES_NAME_LEN];
    bool http_advert;
    char dhcp_name[NETNAMES_NAME_LEN];
} netnames_settings_t;

#ifdef __cplusplus
}
#endif
