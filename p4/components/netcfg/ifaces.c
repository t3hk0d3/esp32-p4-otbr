// The netifs of the board (spec 5.13): created from the configuration on one Ethernet driver, driven by ETH_EVENT, registered
// with mDNS, and the mapping from a request's local address to an interface. Thin glue; the decisions are in the pure files.
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

#include "esp_check.h"
#include "esp_eth.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_net_stack.h"
#include "freertos/FreeRTOS.h"
#include "lwip/dhcp.h"
#include "lwip/sockets.h"
#include "mdns.h"
#include "netcfg_glue.h"
#include "netcfg_mdns.h"
#include "netcfg_priv.h"

static const char *TAG = "netcfg_if";

#define KEY_SINGLE "ETH_DEF" /* the stock key: other code and the logs know the single interface by it */
#define KEY_MGMT "ETH_MGMT"
#define KEY_BACKBONE "ETH_BB"
#define NAME_SINGLE "eth"
#define NAME_MGMT "mgmt"
#define NAME_BACKBONE "thread_bb"
#define OT_IFKEY "OT_DEF"
#define ROUTE_PRIO_BACKBONE 50 /* the default route goes to the backbone (NAT64 egress needs it) */
#define ROUTE_PRIO_MGMT 40
#define LINK_SPEED_100M 100000000
#define LINK_SPEED_10M 10000000
#define LINK_SPEED_1000M 1000000000
#define IP6_BYTES 16
#define IP6_LINKLOCAL_PREFIX 0xfe
#define IP6_LINKLOCAL_MASK 0xc0
#define IP6_LINKLOCAL_BITS 0x80

typedef struct {
    char name[NETCFG_IFACE_NAME_MAX];
    netcfg_side_t side;
    uint16_t vlan;
    esp_netif_t *netif;
    netcfg_vlan_glue_t vlan_glue;
    esp_eth_netif_glue_handle_t stock_glue; /* untagged interfaces */
    bool mdns;                              /* registered with mDNS (netcfg_mdns_allowed) */
} iface_t;

static struct {
    esp_eth_handle_t eth;
    netcfg_mode_t mode;
    iface_t iface[NETCFG_MAX_IFACES];
    size_t count;
    size_t mgmt;
    size_t backbone;
    bool link;
    bool created;
    bool handlers;       /* ETH / IP event handlers registered */
} s_net = {.mode = NETCFG_MODE_SINGLE};

/* DHCP client name of every interface (option 12); empty = lwIP's default. Set from any task, read when a netif is made. */
static char s_dhcp_name[NETCFG_DHCP_NAME_MAX + 1];
static portMUX_TYPE s_dhcp_name_lock = portMUX_INITIALIZER_UNLOCKED;

/* ---- creation ---- */

static esp_err_t apply_static(esp_netif_t *netif, const netcfg_iface_cfg_t *cfg)
{
    const esp_netif_ip_info_t info = {.ip = cfg->addr, .netmask = cfg->mask, .gw = cfg->gw};
    const esp_err_t stopped = esp_netif_dhcpc_stop(netif);

    if (stopped != ESP_OK && stopped != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        ESP_RETURN_ON_ERROR(stopped, TAG, "stopping the DHCP client failed");
    }
    ESP_RETURN_ON_ERROR(esp_netif_set_ip_info(netif, &info), TAG, "setting the static address failed");
    if (cfg->dns.addr != 0) {
        esp_netif_dns_info_t dns = {.ip = {.type = ESP_IPADDR_TYPE_V4, .u_addr.ip4 = cfg->dns}};

        ESP_RETURN_ON_ERROR(esp_netif_set_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns), TAG, "setting the DNS server failed");
    }
    return ESP_OK;
}

static esp_err_t add_iface(const char *name, const char *key, int route_prio, netcfg_side_t side, const netcfg_iface_cfg_t *cfg)
{
    ESP_RETURN_ON_FALSE(s_net.count < NETCFG_MAX_IFACES, ESP_ERR_NO_MEM, TAG, "too many interfaces");
    iface_t *iface = &s_net.iface[s_net.count];
    esp_netif_inherent_config_t base = ESP_NETIF_INHERENT_DEFAULT_ETH();
    base.if_key = key;
    base.if_desc = name;
    base.route_prio = route_prio;
    const esp_netif_config_t config = {.base = &base, .driver = NULL, .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH};

    strlcpy(iface->name, name, sizeof(iface->name));
    iface->side = side;
    iface->vlan = cfg->vlan;
    iface->netif = esp_netif_new(&config);
    ESP_RETURN_ON_FALSE(iface->netif, ESP_ERR_NO_MEM, TAG, "creating netif %s failed", name);
    char dhcp_name[sizeof(s_dhcp_name)];

    taskENTER_CRITICAL(&s_dhcp_name_lock);
    strlcpy(dhcp_name, s_dhcp_name, sizeof(dhcp_name));
    taskEXIT_CRITICAL(&s_dhcp_name_lock);
    s_net.count++; // counted from here on, so that netcfg_ifaces_destroy also undoes a half-built interface
    // A name the netif refuses must never cost the interface: lwIP's default name is used instead.
    if (dhcp_name[0] && esp_netif_set_hostname(iface->netif, dhcp_name) != ESP_OK) {
        ESP_LOGW(TAG, "%s: DHCP name \"%s\" refused, the default is used", name, dhcp_name);
    }
    if (cfg->vlan == NETCFG_VLAN_UNTAGGED) {
        iface->stock_glue = esp_eth_new_netif_glue(s_net.eth);
        ESP_RETURN_ON_FALSE(iface->stock_glue, ESP_FAIL, TAG, "creating the netif glue of %s failed", name);
        ESP_RETURN_ON_ERROR(esp_netif_attach(iface->netif, iface->stock_glue), TAG, "attaching %s failed", name);
    } else {
        netcfg_vlan_glue_init(&iface->vlan_glue, s_net.eth, cfg->vlan);
        ESP_RETURN_ON_ERROR(esp_netif_attach(iface->netif, &iface->vlan_glue), TAG, "attaching %s failed", name);
    }
    if (!cfg->dhcp) {
        ESP_RETURN_ON_ERROR(apply_static(iface->netif, cfg), TAG, "static address of %s failed", name);
    }
    ESP_LOGI(TAG, "%s: %s, %s", name, cfg->vlan ? "tagged" : "untagged", cfg->dhcp ? "DHCPv4" : "static IPv4");
    return ESP_OK;
}

static esp_err_t install_demux(void)
{
    netcfg_demux_t table = {.count = s_net.count};
    esp_netif_t *netifs[NETCFG_MAX_IFACES] = {0};

    for (size_t i = 0; i < s_net.count; i++) {
        table.vlan[i] = s_net.iface[i].vlan;
        netifs[i] = s_net.iface[i].netif;
    }
    return netcfg_vlan_install_demux(s_net.eth, &table, netifs);
}

esp_err_t netcfg_ifaces_create(esp_eth_handle_t eth, const netcfg_config_t *cfg)
{
    ESP_RETURN_ON_FALSE(!s_net.created, ESP_ERR_INVALID_STATE, TAG, "already created");
    s_net.eth = eth;
    s_net.mode = cfg->mode;
    if (cfg->mode == NETCFG_MODE_SINGLE) {
        ESP_RETURN_ON_ERROR(add_iface(NAME_SINGLE, KEY_SINGLE, ROUTE_PRIO_BACKBONE, NETCFG_SIDE_MGMT, &cfg->single), TAG, "single");
        s_net.mgmt = s_net.backbone = 0;
    } else {
        ESP_RETURN_ON_ERROR(add_iface(NAME_MGMT, KEY_MGMT, ROUTE_PRIO_MGMT, NETCFG_SIDE_MGMT, &cfg->mgmt), TAG, "mgmt");
        s_net.mgmt = 0;
        ESP_RETURN_ON_ERROR(add_iface(NAME_BACKBONE, KEY_BACKBONE, ROUTE_PRIO_BACKBONE, NETCFG_SIDE_THREAD, &cfg->thread_bb), TAG,
                            "thread_bb");
        s_net.backbone = 1;
    }
    // The stock glue of an untagged interface installed its own input path; a plain untagged single interface keeps it
    // (the boot behaves exactly as before netcfg). Anything with a tag needs the demux, which comes after all attaches.
    if (s_net.mode == NETCFG_MODE_SPLIT || s_net.iface[0].vlan != NETCFG_VLAN_UNTAGGED) {
        ESP_RETURN_ON_ERROR(install_demux(), TAG, "installing the VLAN demux failed");
    }
    if (s_net.mode == NETCFG_MODE_SPLIT) { // the default route goes to the backbone; one netif becomes the default by itself
        esp_netif_set_default_netif(s_net.iface[s_net.backbone].netif);
    }
    netcfg_ip6_hooks_configure(s_net.mode == NETCFG_MODE_SPLIT ? s_net.iface[s_net.mgmt].netif : NULL, s_net.iface[s_net.backbone].netif);
    s_net.created = true;
    return ESP_OK;
}

/* ---- events ---- */

static iface_t *iface_of(const esp_netif_t *netif)
{
    for (size_t i = 0; i < s_net.count; i++) {
        if (s_net.iface[i].netif == netif) {
            return &s_net.iface[i];
        }
    }
    return NULL;
}

static void set_link_speed(esp_netif_t *netif)
{
    eth_speed_t speed = ETH_SPEED_100M;

    esp_eth_ioctl(s_net.eth, ETH_CMD_G_SPEED, &speed);
    esp_netif_set_link_speed(netif, speed == ETH_SPEED_1000M ? LINK_SPEED_1000M : speed == ETH_SPEED_100M ? LINK_SPEED_100M : LINK_SPEED_10M);
}

// The stock glue drives the untagged interface (its handlers were registered first, so they have run); a tagged one is driven here.
static void on_eth_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (*(esp_eth_handle_t *)data != s_net.eth) {
        return;
    }
    for (size_t i = 0; i < s_net.count; i++) {
        iface_t *iface = &s_net.iface[i];
        const bool tagged = iface->vlan != NETCFG_VLAN_UNTAGGED;

        switch (id) {
        case ETHERNET_EVENT_START:
            if (tagged) {
                esp_netif_action_start(iface->netif, base, id, data);
            }
            break;
        case ETHERNET_EVENT_STOP:
            if (tagged) {
                esp_netif_action_stop(iface->netif, base, id, data);
            }
            break;
        case ETHERNET_EVENT_CONNECTED:
            if (tagged) {
                set_link_speed(iface->netif);
                esp_netif_action_connected(iface->netif, base, id, data);
            }
            // Link-local is not created automatically; SLAAC follows once a router advertisement arrives.
            if (esp_netif_create_ip6_linklocal(iface->netif) != ESP_OK) {
                ESP_LOGW(TAG, "creating the IPv6 link-local address of %s failed", iface->name);
            }
            break;
        case ETHERNET_EVENT_DISCONNECTED:
            if (tagged) {
                esp_netif_action_disconnected(iface->netif, base, id, data);
            }
            if (iface->mdns) {
                mdns_netif_action(iface->netif, MDNS_EVENT_DISABLE_IP4);
                mdns_netif_action(iface->netif, MDNS_EVENT_DISABLE_IP6);
            }
            break;
        default:
            break;
        }
    }
    if (id == ETHERNET_EVENT_CONNECTED) {
        s_net.link = true;
    } else if (id == ETHERNET_EVENT_DISCONNECTED || id == ETHERNET_EVENT_STOP) {
        s_net.link = false;
    }
}

// mDNS answers on the interface it is registered with (netcfg_mdns_allowed) once it has an address (IP events carry the netif first).
static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    const ip_event_got_ip_t *event = data;
    const iface_t *iface = iface_of(event->esp_netif);

    if (!iface || !iface->mdns) {
        return;
    }
    if (id == IP_EVENT_GOT_IP6) {
        mdns_netif_action(event->esp_netif, MDNS_EVENT_ENABLE_IP6);
        mdns_netif_action(event->esp_netif, MDNS_EVENT_ANNOUNCE_IP6);
    } else {
        mdns_netif_action(event->esp_netif, MDNS_EVENT_ENABLE_IP4);
    }
}

esp_err_t netcfg_ifaces_start(void)
{
    ESP_RETURN_ON_FALSE(s_net.created, ESP_ERR_INVALID_STATE, TAG, "no interfaces");
    for (size_t i = 0; i < s_net.count; i++) {
        iface_t *iface = &s_net.iface[i];

        if (!netcfg_mdns_allowed(s_net.mode, iface->side)) {
            // split mode: mgmt is never registered, so it neither announces, answers nor joins the multicast groups
            ESP_LOGI(TAG, "mDNS not on %s", iface->name);
            continue;
        }
        // mDNS (started early by app_main) is not running when its start failed; then there is simply no mDNS.
        const esp_err_t err = mdns_register_netif(iface->netif);

        if (err == ESP_OK) {
            iface->mdns = true;
            ESP_LOGI(TAG, "mDNS active on %s", iface->name);
        } else {
            ESP_LOGW(TAG, "mDNS on %s unavailable: %s", iface->name, esp_err_to_name(err));
        }
    }
    s_net.handlers = true; // set first: a partial registration is undone by netcfg_ifaces_destroy
    ESP_RETURN_ON_ERROR(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, on_eth_event, NULL), TAG, "ETH event handler failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, on_ip_event, NULL), TAG, "IP event handler failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_GOT_IP6, on_ip_event, NULL), TAG, "IP event handler failed");
    return esp_eth_start(s_net.eth);
}

void netcfg_ifaces_destroy(void)
{
    if (s_net.handlers) {
        esp_eth_stop(s_net.eth);
        esp_event_handler_unregister(ETH_EVENT, ESP_EVENT_ANY_ID, on_eth_event);
        esp_event_handler_unregister(IP_EVENT, IP_EVENT_ETH_GOT_IP, on_ip_event);
        esp_event_handler_unregister(IP_EVENT, IP_EVENT_GOT_IP6, on_ip_event);
    }
    for (size_t i = 0; i < s_net.count; i++) {
        iface_t *iface = &s_net.iface[i];

        if (iface->mdns) {
            mdns_unregister_netif(iface->netif);
        }
        if (iface->stock_glue) {
            esp_eth_del_netif_glue(iface->stock_glue);
        }
        esp_netif_destroy(iface->netif);
    }
    netcfg_ip6_hooks_configure(NULL, NULL);
    memset(&s_net.iface, 0, sizeof(s_net.iface));
    s_net.count = 0;
    s_net.created = s_net.handlers = s_net.link = false;
    s_net.mode = NETCFG_MODE_SINGLE;
}

/* ---- queries ---- */

esp_netif_t *netcfg_backbone_netif(void)
{
    return s_net.created ? s_net.iface[s_net.backbone].netif : NULL;
}

esp_netif_t *netcfg_mgmt_netif(void)
{
    return s_net.created ? s_net.iface[s_net.mgmt].netif : NULL;
}

/*
 * Runs on the TCP/IP thread: a DHCPREQUEST that carries the new name, the lease and its address stay. Only with a lease:
 * dhcp_renew forces the RENEWING state whatever the client was doing, and without a lease nothing would ever take it out
 * of it again. A client still looking for a lease sends the new name with its next DISCOVER or REQUEST anyway.
 */
static esp_err_t renew_lease(void *ctx)
{
    struct netif *netif = esp_netif_get_netif_impl(ctx);

    if (!netif || !netif_dhcp_data(netif) || !dhcp_supplied_address(netif)) {
        return ESP_OK;
    }
    return dhcp_renew(netif) == ERR_OK ? ESP_OK : ESP_FAIL;
}

esp_err_t netcfg_ifaces_set_dhcp_name(const char *name)
{
    esp_err_t result = ESP_OK;

    taskENTER_CRITICAL(&s_dhcp_name_lock);
    strlcpy(s_dhcp_name, name, sizeof(s_dhcp_name));
    taskEXIT_CRITICAL(&s_dhcp_name_lock);
    for (size_t i = 0; i < s_net.count; i++) {
        esp_netif_t *netif = s_net.iface[i].netif;
        esp_netif_dhcp_status_t status = ESP_NETIF_DHCP_INIT;
        esp_err_t err = esp_netif_set_hostname(netif, name);

        if (err == ESP_OK && esp_netif_dhcpc_get_status(netif, &status) == ESP_OK && status == ESP_NETIF_DHCP_STARTED &&
                esp_netif_is_netif_up(netif)) {
            err = esp_netif_tcpip_exec(renew_lease, netif);
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "DHCP name of %s: %s", s_net.iface[i].name, esp_err_to_name(err));
            result = err;
        }
    }
    return result;
}

netcfg_mode_t netcfg_ifaces_mode(void)
{
    return s_net.mode;
}

size_t netcfg_ifaces_state(netcfg_iface_state_t out[NETCFG_MAX_IFACES])
{
    for (size_t i = 0; i < s_net.count; i++) {
        const iface_t *iface = &s_net.iface[i];
        esp_netif_ip_info_t ip4;
        esp_ip6_addr_t ip6[NETCFG_IP6_MAX];

        memset(&out[i], 0, sizeof(out[i]));
        strlcpy(out[i].name, iface->name, sizeof(out[i].name));
        out[i].vlan = iface->vlan;
        out[i].link = s_net.link;
        if (esp_netif_get_ip_info(iface->netif, &ip4) == ESP_OK && ip4.ip.addr != 0) {
            snprintf(out[i].ipv4, sizeof(out[i].ipv4), IPSTR, IP2STR(&ip4.ip));
        }
        const int count = esp_netif_get_all_ip6(iface->netif, ip6);

        for (int k = 0; k < count && out[i].ipv6_count < NETCFG_IP6_MAX; k++) {
            snprintf(out[i].ipv6[out[i].ipv6_count++], sizeof(out[i].ipv6[0]), IPV6STR, IPV62STR(ip6[k]));
        }
    }
    return s_net.count;
}

/* ---- request -> interface ---- */

static bool has_v4(esp_netif_t *netif, esp_ip4_addr_t addr)
{
    esp_netif_ip_info_t info;

    return esp_netif_get_ip_info(netif, &info) == ESP_OK && info.ip.addr == addr.addr;
}

static bool has_v6(esp_netif_t *netif, const uint8_t addr[IP6_BYTES])
{
    esp_ip6_addr_t list[NETCFG_IP6_MAX];
    const int count = esp_netif_get_all_ip6(netif, list);

    for (int i = 0; i < count; i++) {
        if (memcmp(list[i].addr, addr, IP6_BYTES) == 0) {
            return true;
        }
    }
    return false;
}

static bool is_link_local(const uint8_t addr[IP6_BYTES])
{
    return addr[0] == IP6_LINKLOCAL_PREFIX && (addr[1] & IP6_LINKLOCAL_MASK) == IP6_LINKLOCAL_BITS;
}

// A link-local address is the same on both interfaces (same MAC), the zone tells them apart.
static bool match_v6(const uint8_t addr[IP6_BYTES], uint32_t scope_id, netcfg_side_t *side)
{
    for (size_t i = 0; i < s_net.count; i++) {
        const iface_t *iface = &s_net.iface[i];

        if (is_link_local(addr) && (int)scope_id != esp_netif_get_netif_impl_index(iface->netif)) {
            continue;
        }
        if (has_v6(iface->netif, addr)) {
            *side = iface->side;
            return true;
        }
    }
    esp_netif_t *ot = esp_netif_get_handle_from_ifkey(OT_IFKEY);

    if (ot && !is_link_local(addr) && has_v6(ot, addr)) { /* OpenThread addresses count as thread_bb */
        *side = NETCFG_SIDE_THREAD;
        return true;
    }
    return false;
}

static bool match_v4(esp_ip4_addr_t addr, netcfg_side_t *side)
{
    for (size_t i = 0; i < s_net.count; i++) {
        if (has_v4(s_net.iface[i].netif, addr)) {
            *side = s_net.iface[i].side;
            return true;
        }
    }
    return false;
}

bool netcfg_ifaces_request_side(httpd_req_t *req, netcfg_side_t *side)
{
    struct sockaddr_storage local;
    socklen_t len = sizeof(local);

    if (getsockname(httpd_req_to_sockfd(req), (struct sockaddr *)&local, &len) != 0) {
        return false;
    }
    if (local.ss_family == AF_INET) {
        const struct sockaddr_in *v4 = (const struct sockaddr_in *)&local;

        return match_v4((esp_ip4_addr_t) {
            .addr = v4->sin_addr.s_addr
        }, side);
    }
    if (local.ss_family != AF_INET6) {
        return false;
    }
    // The server's socket is dual-stack: an IPv4 connection shows as ::ffff:a.b.c.d (validated in Phase 0).
    const struct sockaddr_in6 *v6 = (const struct sockaddr_in6 *)&local;
    esp_ip4_addr_t mapped;

    if (netcfg_v4_from_mapped(v6->sin6_addr.s6_addr, &mapped)) {
        return match_v4(mapped, side);
    }
    return match_v6(v6->sin6_addr.s6_addr, v6->sin6_scope_id, side);
}
