// Own-announcement filter for mDNS (spec 5.13). The mDNS library binds one UDP PCB to port 5353 and installs its receive
// callback with udp_recv(); lwIP has no input hook for IPv4 and the library offers no packet callback. The linker option
// --wrap=udp_recv (CMakeLists.txt) puts a filter in front of that callback: a datagram whose source is an address of another
// interface of the board is our own announcement coming back through a reflector and is dropped before the library sees it
// (it would read it as a host name conflict and rename the host). The decision is netcfg_mdns_source_is_foreign_own().
#include <string.h>

#include "lwip/ip_addr.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/udp.h"
#include "netcfg_glue.h"
#include "netcfg_mdns.h"

#define MDNS_PORT 5353
#define NETIF_SCAN_MAX 6 /* netifs whose addresses are compared: two Ethernet, OpenThread, loopback, spare */
#define ADDR_MAX (NETIF_SCAN_MAX * (1 + LWIP_IPV6_NUM_ADDRESSES))

void __real_udp_recv(struct udp_pcb *pcb, udp_recv_fn recv, void *recv_arg);

// The filter runs in the tcpip thread only, like the callback it guards.
static struct udp_pcb *s_pcb;
static udp_recv_fn s_recv;
static netcfg_addr_t s_input[1 + LWIP_IPV6_NUM_ADDRESSES];
static netcfg_addr_t s_other[ADDR_MAX];

static netcfg_addr_t from_ip(const ip_addr_t *ip)
{
    netcfg_addr_t addr = {0};

    if (IP_IS_V6(ip)) {
        addr.is_v6 = true;
        memcpy(addr.bytes, ip_2_ip6(ip)->addr, sizeof(addr.bytes));
    } else {
        memcpy(addr.bytes, &ip_2_ip4(ip)->addr, sizeof(ip_2_ip4(ip)->addr));
    }
    return addr;
}

static size_t collect(const struct netif *netif, netcfg_addr_t *out, size_t room)
{
    size_t count = 0;

    if (!ip4_addr_isany_val(*netif_ip4_addr(netif)) && count < room) {
        const ip_addr_t ip = IPADDR4_INIT(netif_ip4_addr(netif)->addr);

        out[count++] = from_ip(&ip);
    }
    for (int i = 0; i < LWIP_IPV6_NUM_ADDRESSES && count < room; i++) {
        if (ip6_addr_isvalid(netif_ip6_addr_state(netif, i))) {
            ip_addr_t ip;

            ip_addr_copy_from_ip6(ip, *netif_ip6_addr(netif, i));
            out[count++] = from_ip(&ip);
        }
    }
    return count;
}

static void mdns_input_filter(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, u16_t port)
{
    const struct netif *input = ip_current_input_netif();
    const netcfg_addr_t src = from_ip(addr);
    size_t input_count = input ? collect(input, s_input, sizeof(s_input) / sizeof(s_input[0])) : 0;
    size_t other_count = 0;
    struct netif *netif;

    NETIF_FOREACH(netif) {
        if (netif != input) {
            other_count += collect(netif, s_other + other_count, ADDR_MAX - other_count);
        }
    }
    if (netcfg_mdns_source_is_foreign_own(&src, s_input, input_count, s_other, other_count)) {
        pbuf_free(p);
        return;
    }
    s_recv(arg, pcb, p, addr, port);
}

void __wrap_udp_recv(struct udp_pcb *pcb, udp_recv_fn recv, void *recv_arg)
{
    if (pcb && recv && pcb->local_port == MDNS_PORT && !s_pcb) {
        s_pcb = pcb;
        s_recv = recv;
        __real_udp_recv(pcb, mdns_input_filter, recv_arg);
    } else {
        if (pcb && pcb == s_pcb) { // the library removes or replaces its callback
            s_pcb = NULL;
            s_recv = NULL;
        }
        __real_udp_recv(pcb, recv, recv_arg);
    }
}
