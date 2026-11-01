#pragma once

// Per-interface service policy (spec 5.13, rule 3). Pure; host-tested in test_host/.

#include <stdbool.h>
#include <stdint.h>
#include "netcfg_config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NETCFG_SIDE_MGMT,   /**< the management interface (the only one in single mode) */
    NETCFG_SIDE_THREAD, /**< the Thread backbone interface, or the OpenThread netif */
} netcfg_side_t;

typedef enum {
    NETCFG_SVC_GUI,         /**< static files and Web GUI endpoints */
    NETCFG_SVC_DEVICE_API,  /**< /device/ * */
    NETCFG_SVC_REST_READ,   /**< Thread REST GET / HEAD */
    NETCFG_SVC_REST_WRITE,  /**< every other Thread REST method */
} netcfg_service_t;

/** Single mode allows everything. Split mode: mgmt allows GUI, DEVICE_API, REST_READ; thread allows REST_READ, REST_WRITE. */
bool netcfg_policy_allows(netcfg_mode_t mode, netcfg_side_t side, netcfg_service_t svc);

/** A trial is confirmed only by a request that arrived on the management side. */
bool netcfg_confirm_allowed(netcfg_side_t side);

/** The IPv4 address inside an IPv4-mapped IPv6 address (::ffff:a.b.c.d, 16 bytes) as a network-order address; false if it is not one. */
bool netcfg_v4_from_mapped(const uint8_t ip6[16], esp_ip4_addr_t *out);

#ifdef __cplusplus
}
#endif
