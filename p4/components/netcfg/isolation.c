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
