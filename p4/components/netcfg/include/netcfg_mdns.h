#pragma once

// mDNS rules of the board (spec 5.13): on which interface mDNS runs, and which received mDNS packets are our own
// announcements coming back through a reflector. Pure; the glue is in ifaces.c and mdns_filter.c. Host-tested in test_host/.

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include "netcfg_policy.h"

#ifdef __cplusplus
extern "C" {
#endif

/** An IP address as bytes in network order: IPv4 in the first four bytes of `bytes`. */
typedef struct {
    bool is_v6;
    uint8_t bytes[16];
} netcfg_addr_t;

/**
 * Is mDNS (hostname and every service) active on an interface? Single mode: yes. Split mode: only on the Thread side
 * (thread_bb); never on mgmt, which is not registered with the mDNS library at all.
 */
bool netcfg_mdns_allowed(netcfg_mode_t mode, netcfg_side_t side);

/**
 * Is a received mDNS packet our own announcement coming back (a reflector between two VLANs)? True when the source is an
 * address of another interface of the device (`other`) and not also one of the interface it arrived on (`input`): the
 * latter is the normal loopback of our own multicast, which the mDNS library handles. IPv4-mapped IPv6 sources compare as
 * IPv4; unspecified addresses never match.
 */
bool netcfg_mdns_source_is_foreign_own(const netcfg_addr_t *src, const netcfg_addr_t *input, size_t input_count,
                                       const netcfg_addr_t *other, size_t other_count);

#ifdef __cplusplus
}
#endif
