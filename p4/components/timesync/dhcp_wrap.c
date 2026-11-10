// The DHCP lease's NTP servers (option 42). lwIP hands them to its SNTP module through dhcp_set_ntp_servers, which has
// no interface argument; the link option --wrap=dhcp_set_ntp_servers (CMakeLists.txt) puts this function in front of
// it. It runs on lwIP's thread inside dhcp_recv, where ip_current_input_netif() is the interface the lease came on.
// SNTP's own DHCP mode stays off, so the real function changes nothing; it is called for completeness.

#include "esp_netif.h"
#include "esp_netif_net_stack.h"
#include "lwip/dhcp.h"
#include "lwip/ip.h"
#include "netcfg.h"
#include "timesync_priv.h"

void __real_dhcp_set_ntp_servers(u8_t num, const ip4_addr_t *servers);

// The esp_netif of one of netcfg's interfaces whose lwIP netif is `netif`; NULL when none.
static esp_netif_t *esp_netif_of(const struct netif *netif)
{
    esp_netif_t *const candidates[] = {netcfg_mgmt_netif(), netcfg_backbone_netif()};

    for (size_t i = 0; netif && i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        if (candidates[i] && esp_netif_get_netif_impl(candidates[i]) == netif) {
            return candidates[i];
        }
    }
    return NULL;
}

void __wrap_dhcp_set_ntp_servers(u8_t num, const ip4_addr_t *servers)
{
    uint32_t addrs[TIMESYNC_MAX_SERVERS];
    const uint8_t count = num > TIMESYNC_MAX_SERVERS ? TIMESYNC_MAX_SERVERS : num;

    for (uint8_t i = 0; i < count; i++) {
        addrs[i] = ip4_addr_get_u32(&servers[i]);
    }
    timesync_note_dhcp(count, addrs, timesync_iface_of(esp_netif_of(ip_current_input_netif())));
    __real_dhcp_set_ntp_servers(num, servers);
}
