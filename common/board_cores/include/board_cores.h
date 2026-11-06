#pragma once

// Core placement (spec 5.11). Every task we create and every pinning knob uses these names, never a literal.
//
//   NET_CORE  (0)  network path: lwIP tcpip (CONFIG_LWIP_TCPIP_TASK_AFFINITY_CPU0), EMAC RX task and ISR
//                  (ETH_MAC_FLAG_PIN_TO_CORE, driver installed from a NET_CORE task), OpenThread task and
//                  br_bringup, net_init, default esp_event loop (task_core_id 0 in IDF), mDNS
//                  (CONFIG_MDNS_TASK_AFFINITY_CPU0), esp_timer task.
//   MGMT_CORE (1)  management: HTTP server (httpd_config_t.core_id, patch 1), thread_rest worker (patch 11),
//                  device_api workers (pcap_stream, ota_http, radio_fw, rom_recover), C6 log receiver,
//                  diag sampler, console_repl (main/console.c), the rest of app_main (CONFIG_ESP_MAIN_TASK_AFFINITY_CPU1),
//                  routing (the BGP and RIPng sockets), the radio channel mux: radio_mux (owner task), usb_lib (USB
//                  host daemon) and USB-CDC (CDC driver task, cdc_acm_host_driver_config_t.xCoreID); usb_host_install
//                  is called from the radio_mux task, so the USB interrupt lands on core 1 too.
//
// Note: the C6 log UART driver is first installed from net_init (core 0); after a pause/resume handoff the log task
// (core 1) reinstalls it, so its ISR then lives on core 1. Harmless: it is management traffic.
//
// Tasks of ESP-IDF or of vendored code that no knob pins may float ("any" in /device/diag): ipc*, IDLE*
// are per-core by design; test-only tasks and the Wi-Fi provisioning DNS task are not part of the table.
#define NET_CORE 0
#define MGMT_CORE 1
