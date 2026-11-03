#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board_cores.h"
#include "br_core.h"
#include "c6_base_image.h"
#include "rcp_sync.h"
#include "device_api.h"
#include "diag.h"
#include "metrics.h"
#include "esp_br_web.h"
#include "console.h"
#include "eth.h"
#include "netcfg.h"
#include "netnames.h"
#include "routing.h"
#include "storage.h"

static const char *TAG = "app";

#define NET_INIT_TASK_STACK 4096
#define NET_INIT_TASK_PRIORITY 5

static char s_web_base_path[] = "/web";

// Drivers allocate their interrupts on the core that installs them: the Ethernet driver and the netifs (netcfg) are set up
// from core 0, and br_core_start (radio sync, OpenThread) runs from the same task on the backbone netif netcfg created.
// It takes a while, so nothing waits for it.
static void net_init_task(void *arg)
{
    esp_eth_handle_t eth = NULL;

    esp_err_t err = eth_driver_start(&eth);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "eth_driver_start failed: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
    }
    err = netcfg_set_dhcp_hostname(netnames_dhcp_name());
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "netcfg_set_dhcp_hostname: %s", esp_err_to_name(err));
    }
    err = netcfg_start(eth);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "netcfg_start failed: %s: no network, the border router is NOT started (HTTP server and console stay; `net reset` + reboot)", esp_err_to_name(err));
        vTaskDelete(NULL);
    }
    netnames_set_mode(netcfg_running_mode());
    br_core_set_trel(netcfg_trel_enabled()); // before the start: applied right after OpenThread is initialised
    err = br_core_start(netcfg_backbone_netif());
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "br_core_start failed: %s", esp_err_to_name(err));
    }
    vTaskDelete(NULL);
}

void app_main(void)
{
    // The sampler (core 1) starts first so that the lock statistics and CPU windows cover the whole boot.
    // Diagnostics are not essential (spec 5.3: validity does not depend on them): log and carry on.
    const esp_err_t diag_err = diag_start();

    if (diag_err != ESP_OK) {
        ESP_LOGE(TAG, "diag_start: %s, /device/diag answers 503", esp_err_to_name(diag_err));
    }
    // The Thread collector of /metrics is not essential either: without it /metrics serves device metrics only.
    const esp_err_t metrics_err = metrics_start();

    if (metrics_err != ESP_OK) {
        ESP_LOGE(TAG, "metrics_start: %s, /metrics has no Thread series", esp_err_to_name(metrics_err));
    }
    // NVS that cannot be initialised (e.g. no free pages) must not boot-loop the board: the rollback target has the
    // same NVS. The device boots degraded (see br_core_note_storage_init); NVS is never erased here.
    br_core_note_storage_init(storage_init());
    // A slot that fails to mount (e.g. a web slot not yet populated) must not stop the network or
    // the firmware update path; storage_mount has already logged the cause.
    esp_err_t err = storage_mount();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "storage_mount: %s", esp_err_to_name(err));
    }
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    // Mesh routing (after metrics_start and NVS, before the border router can come up): br_core hands the
    // post-init steps (OMR config, DHCPv6-PD) to routing_on_border_router_up. Not essential to boot: without the task
    // the hook still applies the stored settings, and the route methods other than PD do not run.
    br_core_set_border_router_up_cb(routing_on_border_router_up);
    err = routing_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "routing_start: %s, RIPng and BGP do not run", esp_err_to_name(err));
    }

    // The HTTP server starts in every mode, RADIO_FAULT and failed network or mDNS start included, and before
    // the radio sync so that it is reachable during a long RCP push. It listens on any address, so it needs no
    // network. The server task runs on core 1 (patch 1) and every handler that touches OpenThread is guarded by
    // the mode (patch 2), so starting it before OpenThread exists is safe.
    // The names (stored or default) before mDNS, the netifs and OpenThread take them.
    err = netnames_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "netnames_init: %s", esp_err_to_name(err));
    }
    err = br_core_mdns_start(netnames_hostname(), netnames_ha_name());
    if (err == ESP_OK) {
        err = netnames_mdns_started();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mDNS start: %s", esp_err_to_name(err));
    }
    // device_api registers its handlers while the server starts, before the catch-all GET handler (patch 10); it
    // needs no OpenThread, so it is also served in RADIO_FAULT.
    device_api_set_base_expected(c6_base_version_start, c6_base_version_end);
    err = rcp_sync_set_base_image(&(const rcp_sync_base_image_t) {
        .bootloader_start = c6_base_bootloader_start, .bootloader_end = c6_base_bootloader_end,
        .ptable_start = c6_base_ptable_start, .ptable_end = c6_base_ptable_end,
        .updater_start = c6_base_updater_start, .updater_end = c6_base_updater_end,
        .version_start = c6_base_version_start, .version_end = c6_base_version_end,
    });
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "the embedded C6 base image is unusable, ROM recovery is off: %s", esp_err_to_name(err));
    }
    esp_br_web_set_extra_handlers(device_api_register);
    esp_br_web_start(s_web_base_path);

    BaseType_t created = xTaskCreatePinnedToCore(net_init_task, "net_init", NET_INIT_TASK_STACK, NULL, NET_INIT_TASK_PRIORITY,
                                                 NULL, NET_CORE);
    ESP_ERROR_CHECK(created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);

    // The console serves `net show` / `net reset` in every build; test builds add their commands to it.
    err = console_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "console_start: %s", esp_err_to_name(err));
    }
    validity_start();
}
