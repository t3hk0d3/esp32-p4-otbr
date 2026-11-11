// Per-interface service policy and the small address helpers of the request-side mapping (spec 5.13). Pure.
#include "netcfg_policy.h"

#include <string.h>

#define IPV4_MAPPED_PREFIX_LEN 12
#define IPV4_LEN 4

bool netcfg_policy_allows(netcfg_mode_t mode, netcfg_side_t side, netcfg_service_t svc)
{
    if (mode != NETCFG_MODE_SPLIT) {
        return true;
    }
    if (side == NETCFG_SIDE_MGMT) {
        return svc == NETCFG_SVC_GUI || svc == NETCFG_SVC_DEVICE_API || svc == NETCFG_SVC_REST_READ || svc == NETCFG_SVC_RCP_PROXY ||
               svc == NETCFG_SVC_USBIP;
    }
    return svc == NETCFG_SVC_REST_READ || svc == NETCFG_SVC_REST_WRITE || svc == NETCFG_SVC_RCP_PROXY;
}

bool netcfg_policy_conn_allowed(netcfg_mode_t mode, bool found, netcfg_side_t side, bool on_mesh, netcfg_service_t svc)
{
    if (netcfg_mesh_refused(on_mesh)) {
        return false;
    }
    if (mode != NETCFG_MODE_SPLIT) {
        return true;
    }
    return found && netcfg_policy_allows(mode, side, svc);
}

bool netcfg_mesh_refused(bool on_mesh)
{
    return on_mesh;
}

bool netcfg_confirm_allowed(netcfg_side_t side)
{
    return side == NETCFG_SIDE_MGMT;
}

bool netcfg_v4_from_mapped(const uint8_t ip6[16], esp_ip4_addr_t *out)
{
    static const uint8_t prefix[IPV4_MAPPED_PREFIX_LEN] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};

    if (memcmp(ip6, prefix, sizeof(prefix)) != 0) {
        return false;
    }
    memcpy(&out->addr, ip6 + IPV4_MAPPED_PREFIX_LEN, IPV4_LEN);
    return true;
}
