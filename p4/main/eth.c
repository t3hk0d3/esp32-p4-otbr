#include "eth.h"

#include "board.h"
#include "esp_check.h"
#include "esp_eth.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"

static const char *TAG = "eth";

static void on_eth_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    switch (id) {
    case ETHERNET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "link up");
        break;
    case ETHERNET_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "link down");
        break;
    default:
        break;
    }
}

static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    const ip_event_got_ip_t *event = data;

    ESP_LOGI(TAG, "got ip " IPSTR, IP2STR(&event->ip_info.ip));
}

static void on_got_ip6(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    const ip_event_got_ip6_t *event = data;
    const bool link_local = esp_netif_ip6_get_addr_type(&event->ip6_info.ip) == ESP_IP6_ADDR_IS_LINK_LOCAL;

    ESP_LOGI(TAG, "got ip6 " IPV6STR " %s", IPV62STR(event->ip6_info.ip), link_local ? "link-local" : "slaac");
}

static esp_err_t install_driver(esp_eth_handle_t *out_handle)
{
    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    mac_config.flags |= ETH_MAC_FLAG_PIN_TO_CORE; // EMAC RX task on the core that installs the driver

    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = P4_ETH_PHY_ADDR;
    phy_config.reset_gpio_num = P4_PIN_ETH_PHY_RST;

    eth_esp32_emac_config_t emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    emac_config.smi_gpio.mdc_num = P4_PIN_ETH_MDC;
    emac_config.smi_gpio.mdio_num = P4_PIN_ETH_MDIO;
    emac_config.interface = EMAC_DATA_INTERFACE_RMII;
    emac_config.clock_config.rmii.clock_mode = EMAC_CLK_EXT_IN;
    emac_config.clock_config.rmii.clock_gpio = P4_PIN_ETH_RMII_CLK;

    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_config, &mac_config);
    ESP_RETURN_ON_FALSE(mac, ESP_FAIL, TAG, "creating MAC failed");
    esp_eth_phy_t *phy = esp_eth_phy_new_generic(&phy_config);
    if (!phy) {
        mac->del(mac);
        ESP_RETURN_ON_FALSE(false, ESP_FAIL, TAG, "creating PHY failed");
    }

    esp_eth_config_t config = ETH_DEFAULT_CONFIG(mac, phy);
    esp_err_t err = esp_eth_driver_install(&config, out_handle);
    if (err != ESP_OK) {
        phy->del(phy);
        mac->del(mac);
    }
    return err;
}

static void unregister_handlers(void)
{
    esp_event_handler_unregister(IP_EVENT, IP_EVENT_GOT_IP6, on_got_ip6);
    esp_event_handler_unregister(IP_EVENT, IP_EVENT_ETH_GOT_IP, on_got_ip);
    esp_event_handler_unregister(ETH_EVENT, ESP_EVENT_ANY_ID, on_eth_event);
}

static void uninstall_driver(esp_eth_handle_t handle)
{
    esp_eth_mac_t *mac = NULL;
    esp_eth_phy_t *phy = NULL;

    esp_eth_get_mac_instance(handle, &mac);
    esp_eth_get_phy_instance(handle, &phy);
    esp_eth_driver_uninstall(handle);
    if (mac) {
        mac->del(mac);
    }
    if (phy) {
        phy->del(phy);
    }
}

esp_err_t eth_driver_start(esp_eth_handle_t *out_eth)
{
    ESP_RETURN_ON_FALSE(out_eth, ESP_ERR_INVALID_ARG, TAG, "no output handle");
    esp_eth_handle_t handle = NULL;

    ESP_RETURN_ON_ERROR(install_driver(&handle), TAG, "installing driver failed");
    esp_err_t err = esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, on_eth_event, NULL);
    if (err == ESP_OK) {
        err = esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, on_got_ip, NULL);
    }
    if (err == ESP_OK) {
        err = esp_event_handler_register(IP_EVENT, IP_EVENT_GOT_IP6, on_got_ip6, NULL);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "registering the event handlers failed: %s", esp_err_to_name(err));
        unregister_handlers();
        uninstall_driver(handle);
        return err;
    }
    ESP_LOGI(TAG, "driver installed, waiting for the interfaces");
    *out_eth = handle;
    return ESP_OK;
}
