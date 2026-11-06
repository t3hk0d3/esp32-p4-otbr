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
    NETCFG_SVC_RCP_PROXY,   /**< the RCP proxy's listen sockets and UDP sources (netcfg_socket_allowed, netcfg_peer_allowed) */
} netcfg_service_t;

/**
 * Single mode allows everything. Split mode: mgmt allows GUI, DEVICE_API, REST_READ, RCP_PROXY; thread allows REST_READ,
 * REST_WRITE, RCP_PROXY.
 */
bool netcfg_policy_allows(netcfg_mode_t mode, netcfg_side_t side, netcfg_service_t svc);

/**
 * The decision for a non-HTTP connection or datagram source (netcfg_socket_allowed, netcfg_peer_allowed): `found` and
 * `side` say which interface it belongs to, `on_mesh` whether it is on the OpenThread netif. The mesh is refused in every
 * mode; otherwise single mode allows everything (also when no interface matches); split mode refuses an unknown interface
 * and applies the policy table.
 */
bool netcfg_policy_conn_allowed(netcfg_mode_t mode, bool found, netcfg_side_t side, bool on_mesh, netcfg_service_t svc);

/** Must a connection be refused because its local address is on the OpenThread netif (the mesh)? The same in every mode. */
bool netcfg_mesh_refused(bool on_mesh);

/** A trial is confirmed only by a request that arrived on the management side. */
bool netcfg_confirm_allowed(netcfg_side_t side);

/** The IPv4 address inside an IPv4-mapped IPv6 address (::ffff:a.b.c.d, 16 bytes) as a network-order address; false if it is not one. */
bool netcfg_v4_from_mapped(const uint8_t ip6[16], esp_ip4_addr_t *out);

#ifdef __cplusplus
}
#endif
