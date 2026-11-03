// IPv6 isolation hooks (spec 5.13, rules 1 and 2). The prebuilt libopenthread_br.a defines lwip_hook_ip6_input and
// lwip_hook_ip6_route (strong symbols that replace IDF's weak defaults), so they are chained with the linker option
// --wrap (CMakeLists.txt; validated in Phase 0) instead of being redefined. The wrappers classify the packet with lwIP and
// apply the verdicts of isolation.c; everything else goes to the original hook. IPv4 needs nothing here: forwarding is off
// (CONFIG_LWIP_IP_FORWARD=n) and ESP-IDF's ip4_route_src_hook already routes by source address.
#include "lwip/ip6.h"
#include "lwip/ip6_addr.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/prot/ip6.h"
#include "sdkconfig.h"
#include "esp_netif.h"
#include "esp_netif_net_stack.h"
#include "netcfg_glue.h"
#include "netcfg_isolation.h"

// Set once, before the Ethernet driver starts and so before any packet is received.
static struct netif *volatile s_mgmt;
static struct netif *volatile s_backbone;

void netcfg_ip6_hooks_configure(esp_netif_t *mgmt, esp_netif_t *backbone)
{
    s_backbone = backbone ? esp_netif_get_netif_impl(backbone) : NULL;
    s_mgmt = mgmt ? esp_netif_get_netif_impl(mgmt) : NULL;
}

int __real_lwip_hook_ip6_input(struct pbuf *p, struct netif *inp);
struct netif *__real_lwip_hook_ip6_route(const ip6_addr_t *src, const ip6_addr_t *dest);

static bool netif_has_addr(struct netif *netif, const ip6_addr_t *addr)
{
    for (int i = 0; i < LWIP_IPV6_NUM_ADDRESSES; i++) {
        if (ip6_addr_isvalid(netif_ip6_addr_state(netif, i)) && ip6_addr_cmp_zoneless(netif_ip6_addr(netif, i), addr)) {
            return true;
        }
    }
    return false;
}

static netcfg_dest_kind_t classify(struct netif *inp, const ip6_addr_t *dest)
{
    if (ip6_addr_ismulticast(dest)) {
        return ip6_addr_multicast_scope(dest) <= IP6_MULTICAST_SCOPE_LINK_LOCAL ? NETCFG_DEST_MULTICAST_LINK : NETCFG_DEST_MULTICAST_WIDE;
    }
    if (netif_has_addr(inp, dest)) {
        return NETCFG_DEST_OF_INPUT_IF;
    }
    if (netif_has_addr(s_mgmt, dest)) {
        return NETCFG_DEST_OF_MGMT_ONLY;
    }
    struct netif *other;

    NETIF_FOREACH(other) {
        if (other != inp && netif_has_addr(other, dest)) {
            return NETCFG_DEST_OF_OTHER_IF;
        }
    }
    return NETCFG_DEST_REMOTE;
}

#if CONFIG_OPENTHREAD_RADIO_TREL
#define TREL_PORT CONFIG_OPENTHREAD_TREL_PORT
#else
#define TREL_PORT 0
#endif

// UDP port rules (netcfg_ip6_udp_drop). The UDP header is looked for behind any extension headers, as lwIP finds it;
// a chain too long to follow may hide a TREL datagram, so on mgmt it is dropped too when TREL is built.
static bool udp_drop(const struct pbuf *p, bool input_is_mgmt)
{
    uint16_t dest_port;

    if (!input_is_mgmt) {
        return false;
    }
    switch (netcfg_ip6_udp_dest_port(p->payload, p->len, &dest_port)) {
    case NETCFG_UDP_PORT_FOUND:
        return netcfg_ip6_udp_drop(true, dest_port, TREL_PORT);
    case NETCFG_UDP_PORT_UNKNOWN:
        return TREL_PORT != 0;
    default:
        return false;
    }
}

int __wrap_lwip_hook_ip6_input(struct pbuf *p, struct netif *inp)
{
    struct netif *mgmt = s_mgmt;

    if (mgmt && p->len >= IP6_HLEN) { // a shorter packet is dropped by ip6_input itself
        const struct ip6_hdr *header = p->payload;
        ip6_addr_t dest;

        ip6_addr_copy_from_packed(dest, header->dest);
        const netcfg_dest_kind_t kind = classify(inp, &dest);
        // ip6_forward routes with an unspecified source: ask the same question, only when the packet could be forwarded.
        const bool forwardable = kind == NETCFG_DEST_REMOTE || kind == NETCFG_DEST_MULTICAST_WIDE;
        const bool route_is_mgmt = inp != mgmt && forwardable && ip6_route(IP6_ADDR_ANY6, &dest) == mgmt;

        if (netcfg_ip6_input_drop(inp == mgmt, kind, route_is_mgmt) || udp_drop(p, inp == mgmt)) {
            pbuf_free(p);
            return 1; // eaten
        }
    }
    return __real_lwip_hook_ip6_input(p, inp);
}

// Does a non-mgmt interface have `dest` on-link? lwIP finds those itself (static addresses imply their /64), so a NULL answer is left alone.
static bool on_link_elsewhere(const ip6_addr_t *dest, const struct netif *mgmt)
{
    struct netif *netif;

    NETIF_FOREACH(netif) {
        for (int i = 0; netif != mgmt && netif_is_up(netif) && i < LWIP_IPV6_NUM_ADDRESSES; i++) {
            if (netif_ip6_addr_isstatic(netif, i) && ip6_addr_netcmp_zoneless(dest, netif_ip6_addr(netif, i))) {
                return true;
            }
        }
    }
    return false;
}

struct netif *__wrap_lwip_hook_ip6_route(const ip6_addr_t *src, const ip6_addr_t *dest)
{
    struct netif *mgmt = s_mgmt;

    if (!mgmt) {
        return __real_lwip_hook_ip6_route(src, dest);
    }
    if (src && netif_has_addr(mgmt, src)) {
        return mgmt; // replies leave by the interface they belong to
    }
    // No other source may resolve to mgmt, also when the original hook says nothing and lwIP would pick a default router
    // learned on mgmt (isolation.c).
    struct netif *original = __real_lwip_hook_ip6_route(src, dest);

    switch (netcfg_ip6_route_choice(true, false, original == mgmt, original == NULL, original == NULL && on_link_elsewhere(dest, mgmt))) {
    case NETCFG_ROUTE_BACKBONE:
        return s_backbone;
    case NETCFG_ROUTE_MGMT:
        return mgmt;
    default:
        return original;
    }
}
