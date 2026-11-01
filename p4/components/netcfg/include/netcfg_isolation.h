#pragma once

// Decisions of the IPv6 isolation hooks (spec 5.13, rules 1 and 2). Pure; the hooks (ip6_hooks.c) classify the packet
// with lwIP and only apply the verdict. Host-tested in test_host/.

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NETCFG_DEST_OF_INPUT_IF,   /**< unicast to an address of the interface the packet arrived on */
    NETCFG_DEST_OF_MGMT_ONLY,  /**< unicast to an address of mgmt, packet arrived elsewhere */
    NETCFG_DEST_OF_OTHER_IF,   /**< unicast to an address of another interface of the device */
    NETCFG_DEST_MULTICAST_LINK, /**< multicast of link-local scope or smaller */
    NETCFG_DEST_MULTICAST_WIDE, /**< multicast of a larger scope */
    NETCFG_DEST_REMOTE,        /**< unicast to nobody on this device */
} netcfg_dest_kind_t;

/**
 * Drop a received packet? On mgmt only traffic to the device itself is kept: its own addresses and link-scope multicast.
 * On the other interfaces a packet is dropped when it is addressed to a mgmt address, or when it would be forwarded
 * (remote destination or wide multicast) out of mgmt (`route_is_mgmt`).
 */
bool netcfg_ip6_input_drop(bool input_is_mgmt, netcfg_dest_kind_t dest, bool route_is_mgmt);

typedef enum {
    NETCFG_ROUTE_ORIGINAL, /**< use the answer of the original hook (lwIP's own routing follows if it is NULL) */
    NETCFG_ROUTE_MGMT,     /**< the mgmt netif */
    NETCFG_ROUTE_BACKBONE, /**< the thread_bb netif: a source that is not mgmt's must never leave through mgmt */
} netcfg_route_choice_t;

/**
 * Route of an IPv6 packet by its source address (split mode). A mgmt source leaves through mgmt. Any other source never
 * resolves to mgmt: when the original hook says mgmt, or says nothing and no other interface has the destination on-link
 * (lwIP would then pick a default router, possibly one learned on mgmt), the backbone is returned. Outside split mode the
 * original answer stands.
 */
netcfg_route_choice_t netcfg_ip6_route_choice(bool split, bool src_is_mgmt, bool original_is_mgmt, bool original_is_null,
                                              bool dest_on_link_elsewhere);

#ifdef __cplusplus
}
#endif
