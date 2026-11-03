// Verdicts of the IPv6 isolation hooks (spec 5.13). Pure.
#include "netcfg_isolation.h"

bool netcfg_ip6_input_drop(bool input_is_mgmt, netcfg_dest_kind_t dest, bool route_is_mgmt)
{
    if (input_is_mgmt) {
        return dest != NETCFG_DEST_OF_INPUT_IF && dest != NETCFG_DEST_MULTICAST_LINK;
    }
    switch (dest) {
    case NETCFG_DEST_OF_MGMT_ONLY:
        return true;
    case NETCFG_DEST_REMOTE:
    case NETCFG_DEST_MULTICAST_WIDE:
        return route_is_mgmt;
    default:
        return false;
    }
}

bool netcfg_ip6_udp_drop(bool input_is_mgmt, uint16_t dest_port, uint16_t trel_port)
{
    return input_is_mgmt && trel_port != 0 && dest_port == trel_port;
}

#define IP6_FIXED_HLEN 40
#define NEXTH_HOP_BY_HOP 0
#define NEXTH_UDP 17
#define NEXTH_ROUTING 43
#define NEXTH_FRAGMENT 44
#define NEXTH_DEST_OPTS 60
#define EXT_HEADERS_MAX 8 // more than any real packet carries; bounds the walk

netcfg_udp_port_t netcfg_ip6_udp_dest_port(const uint8_t *pkt, size_t len, uint16_t *dest_port)
{
    if (len < IP6_FIXED_HLEN) {
        return NETCFG_UDP_PORT_NONE;
    }
    uint8_t next = pkt[6];
    size_t off = IP6_FIXED_HLEN;

    for (int i = 0; i <= EXT_HEADERS_MAX; i++) {
        switch (next) {
        case NEXTH_UDP:
            if (len < off + 8) {
                return NETCFG_UDP_PORT_NONE;
            }
            *dest_port = (uint16_t)((pkt[off + 2] << 8) | pkt[off + 3]);
            return NETCFG_UDP_PORT_FOUND;
        case NEXTH_HOP_BY_HOP:
        case NEXTH_ROUTING:
        case NEXTH_DEST_OPTS:
            if (len < off + 2) {
                return NETCFG_UDP_PORT_NONE;
            }
            next = pkt[off];
            off += ((size_t)pkt[off + 1] + 1) * 8;
            break;
        case NEXTH_FRAGMENT:
            if (len < off + 8 || (((pkt[off + 2] << 8) | pkt[off + 3]) & 0xfff8) != 0) {
                return NETCFG_UDP_PORT_NONE;
            }
            next = pkt[off];
            off += 8;
            break;
        default:
            return NETCFG_UDP_PORT_NONE;
        }
    }
    return NETCFG_UDP_PORT_UNKNOWN;
}

netcfg_route_choice_t netcfg_ip6_route_choice(bool split, bool src_is_mgmt, bool original_is_mgmt, bool original_is_null,
                                              bool dest_on_link_elsewhere)
{
    if (!split) {
        return NETCFG_ROUTE_ORIGINAL;
    }
    if (src_is_mgmt) {
        return NETCFG_ROUTE_MGMT;
    }
    if (original_is_mgmt || (original_is_null && !dest_on_link_elsewhere)) {
        return NETCFG_ROUTE_BACKBONE;
    }
    return NETCFG_ROUTE_ORIGINAL;
}
