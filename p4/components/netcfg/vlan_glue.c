// Driver-level 802.1Q (spec 5.13, validated in Phase 0): a tagged interface has its own esp_netif driver glue that inserts the
// tag on transmit, and one Ethernet input path demultiplexes by VLAN ID. lwIP never sees a VLAN header and no lwIP VLAN
// hook or option is used (they change struct netif / PBUF_LINK_HLEN, which the prebuilt libopenthread_br.a depends on).
//
// IDF v6.1 facts this relies on (read from ~/esp/esp-idf-v6.1/components):
// - esp_eth/src/esp_eth_mac_esp.c:630 hands each received frame (a malloc'ed buffer) to stack_input_info and never frees it
//   afterwards: the input path owns the buffer, also when it drops the frame.
// - esp_eth/src/esp_eth.c:101-125 and :362-373: esp_eth_update_input_path_info() replaces the stack input; the stock glue
//   installs its own in post_attach (esp_eth_netif_glue.c), so the demux is installed after all attaches.
// - esp_netif/lwip/netif/ethernetif.c:133 and esp_pbuf_ref.c:36: lwIP keeps the pointer given to esp_netif_receive() and
//   frees it through driver_free_rx_buffer(): a frame delivered at buf + 4 is freed at buffer - 4.
// - esp_eth/src/mac/esp_eth_mac_esp.c:568-569: esp_eth_transmit_vargs takes (uint8_t *buffer, uint32_t length) pairs.
#include <stdatomic.h>
#include <stdlib.h>

#include "esp_check.h"
#include "esp_eth.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "netcfg_glue.h"

static const char *TAG = "netcfg_vlan";

#define TX_PARTS 3 /* MAC addresses, tag, rest of the frame */

static struct {
    netcfg_demux_t table;
    esp_netif_t *netif[NETCFG_MAX_IFACES];
    atomic_uint_least32_t dropped;
} s_demux;

static esp_err_t vlan_transmit(void *handle, void *buffer, size_t len)
{
    const netcfg_vlan_glue_t *glue = handle;
    uint8_t tag[NETCFG_VLAN_TAG_LEN];

    if (len < NETCFG_MAC_PAIR_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }
    netcfg_vlan_tag(glue->vid, tag);
    return esp_eth_transmit_vargs(glue->eth, TX_PARTS, buffer, (uint32_t)NETCFG_MAC_PAIR_LEN, tag, (uint32_t)NETCFG_VLAN_TAG_LEN,
                                  (uint8_t *)buffer + NETCFG_MAC_PAIR_LEN, (uint32_t)(len - NETCFG_MAC_PAIR_LEN));
}

static void vlan_free_rx_buffer(void *handle, void *buffer)
{
    free((uint8_t *)buffer - NETCFG_VLAN_TAG_LEN); // delivered 4 bytes into the received buffer
}

static esp_err_t vlan_set_mac_filter(void *handle, const uint8_t *mac, size_t mac_len, bool add)
{
    const netcfg_vlan_glue_t *glue = handle;

    ESP_RETURN_ON_FALSE(mac_len == ETH_ADDR_LEN, ESP_ERR_INVALID_ARG, TAG, "invalid MAC length");
    return esp_eth_ioctl(glue->eth, add ? ETH_CMD_ADD_MAC_FILTER : ETH_CMD_DEL_MAC_FILTER, (void *)mac);
}

static esp_err_t vlan_post_attach(esp_netif_t *netif, void *args)
{
    netcfg_vlan_glue_t *glue = args;
    uint8_t mac[ETH_ADDR_LEN];
    const esp_netif_driver_ifconfig_t config = {
        .handle = glue,
        .transmit = vlan_transmit,
        .driver_free_rx_buffer = vlan_free_rx_buffer,
        .driver_set_mac_filter = vlan_set_mac_filter,
    };

    glue->base.netif = netif;
    ESP_RETURN_ON_ERROR(esp_netif_set_driver_config(netif, &config), TAG, "setting the driver config failed");
    ESP_RETURN_ON_ERROR(esp_eth_ioctl(glue->eth, ETH_CMD_G_MAC_ADDR, mac), TAG, "reading the MAC failed");
    return esp_netif_set_mac(netif, mac); // both interfaces share the board's MAC (separate broadcast domains)
}

void netcfg_vlan_glue_init(netcfg_vlan_glue_t *glue, esp_eth_handle_t eth, uint16_t vid)
{
    *glue = (netcfg_vlan_glue_t) {
        .base = {.post_attach = vlan_post_attach}, .eth = eth, .vid = vid
    };
}

static esp_err_t demux_input(esp_eth_handle_t eth, uint8_t *buffer, uint32_t length, void *priv, void *info)
{
    const netcfg_rx_result_t result = netcfg_demux(&s_demux.table, buffer, length);

    if (result.verdict != NETCFG_RX_DELIVER) {
        atomic_fetch_add(&s_demux.dropped, 1);
        free(buffer);
        return ESP_OK;
    }
    return esp_netif_receive(s_demux.netif[result.iface], result.frame, result.len, NULL);
}

esp_err_t netcfg_vlan_install_demux(esp_eth_handle_t eth, const netcfg_demux_t *table, esp_netif_t *const netifs[NETCFG_MAX_IFACES])
{
    ESP_RETURN_ON_FALSE(table->count >= 1 && table->count <= NETCFG_MAX_IFACES, ESP_ERR_INVALID_ARG, TAG, "bad interface count");
    s_demux.table = *table;
    for (size_t i = 0; i < table->count; i++) {
        s_demux.netif[i] = netifs[i];
    }
    return esp_eth_update_input_path_info(eth, demux_input, NULL);
}

uint32_t netcfg_vlan_dropped(void)
{
    return atomic_load(&s_demux.dropped);
}
