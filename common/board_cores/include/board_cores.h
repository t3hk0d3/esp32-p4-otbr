#pragma once

// Core placement (spec 5.11). Every task we create and every pinning knob uses these names, never a literal.
//
//   NET_CORE  (0)  network path: lwIP tcpip (CONFIG_LWIP_TCPIP_TASK_AFFINITY_CPU0), EMAC RX task and ISR
//                  (ETH_MAC_FLAG_PIN_TO_CORE, driver installed from a NET_CORE task), OpenThread task and
//                  br_bringup, net_init, default esp_event loop (task_core_id 0 in IDF), mDNS
//                  (CONFIG_MDNS_TASK_AFFINITY_CPU0), esp_timer task, the radio channel mux's C6 transport:
//                  radio_mux_c6 (the OpenThread task's priority + 1), its SPI2 interrupt (spi_bus_config_t.isr_cpu_id)
//                  and the GPIO ISR service with the C6's INT pin (installed from radio_mux_c6); the mux's USB
//                  transport: usb_lib (USB host daemon, which calls usb_host_install, so the USB interrupt lands on
//                  core 0) and USB-CDC (CDC driver task, cdc_acm_host_driver_config_t.xCoreID). Radio transports stay
//                  off MGMT_CORE: its management code may mask interrupts for hundreds of microseconds (the chip
//                  temperature read), which overran a USB dongle's UART.
//   MGMT_CORE (1)  management: HTTP server (httpd_config_t.core_id, patch 1), thread_rest worker (patch 11),
//                  device_api workers (pcap_stream, ota_http, radio_fw, rom_recover), C6 log receiver,
//                  diag sampler, console_repl (main/console.c), the rest of app_main (CONFIG_ESP_MAIN_TASK_AFFINITY_CPU1),
//                  routing (the BGP and RIPng sockets), the radio channel mux's owner task radio_mux, rcp_proxy (the
//                  rcp-proxy mode's sockets) and rcp_resolve (its connect targets' name lookups); br_radio
//                  (br_core: OpenThread's start and stop on the OTBR channel, moves between channels, the RCP probe).
//
// Note: the C6 log UART driver is first installed from net_init (core 0); after a pause/resume handoff the log task
// (core 1) reinstalls it, so its ISR then lives on core 1. Harmless: it is management traffic.
//
// Tasks of ESP-IDF or of vendored code that no knob pins may float ("any" in /device/diag): ipc*, IDLE*
// are per-core by design; test-only tasks and the Wi-Fi provisioning DNS task are not part of the table.
#define NET_CORE 0
#define MGMT_CORE 1
