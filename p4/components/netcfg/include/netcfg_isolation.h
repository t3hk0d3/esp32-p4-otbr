#pragma once

// Decisions of the IPv6 isolation hooks (spec 5.13, rules 1 and 2). Pure; the hooks (ip6_hooks.c) classify the packet
// with lwIP and only apply the verdict. Host-tested in test_host/.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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

/**
 * Drop a received UDP datagram by its destination port? Split mode keeps the Thread backbone services off mgmt: TREL
 * (`trel_port`, 0 = TREL not built) arriving on mgmt is dropped, even when addressed to mgmt's own address.
 */
bool netcfg_ip6_udp_drop(bool input_is_mgmt, uint16_t dest_port, uint16_t trel_port);

/**
 * Find the UDP destination port of an IPv6 packet (`pkt` starts at the fixed header, `len` bytes are readable), as lwIP
 * finds the UDP header: behind any Hop-by-Hop, Routing, Destination Options and Fragment headers, all within `len`.
 * False when the packet is not UDP, the headers run past `len` (lwIP drops such a packet itself), or it is a
 * non-first fragment (without the first one, which carries the port, it is never reassembled).
 */
bool netcfg_ip6_udp_dest_port(const uint8_t *pkt, size_t len, uint16_t *dest_port);

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
