"""Expected core of each task, mirroring the placement table of spec 5.11 (one source for the scheduling tests).

Names are the FreeRTOS task names as reported in /device/diag tasks[].name:
- tcpip: lwIP (TCPIP_THREAD_NAME, CONFIG_LWIP_TCPIP_TASK_AFFINITY_CPU0)
- emac_rx: esp_eth EMAC RX task (ETH_MAC_FLAG_PIN_TO_CORE, driver installed from the core-0 net_init task)
- ot_main: OpenThread task (br_core, CONFIG_OPENTHREAD_TASK_NAME)
- sys_evt: default esp_event loop (task_core_id 0 in IDF)
- mdns: mDNS service task (CONFIG_MDNS_TASK_AFFINITY_CPU0)
- esp_timer: esp_timer task, always core 0
- httpd: esp_http_server (httpd_config_t.core_id, patch 1); thread_rest: upstream REST worker (patch 11)
- diag, c6_log, pcap_stream, ota_http, radio_fw, rom_recover, radio_mux, rcp_proxy, zigbee, zigbee_tx: our tasks,
  pinned in code (the two zigbee tasks exist only once a channel in mode ziggurat was attached)
- usb_lib, USB-CDC: the USB host daemon and the CDC driver task, pinned by radio_mux
- console_repl: the console REPL (IDF esp_console_repl_chip.c:144; main/console.c pins it)
Tasks that no knob pins (e.g. the Wi-Fi provisioning DNS task) and tasks outside the table are ignored.
"""

NET_CORE = 0
MGMT_CORE = 1

NETWORK_TASKS = {
    "tcpip": NET_CORE,
    "emac_rx": NET_CORE,
    "ot_main": NET_CORE,
    "sys_evt": NET_CORE,
    "mdns": NET_CORE,
    "esp_timer": NET_CORE,
    # Zigbee mode: the zigbee task (ZIGBEE_TASK_CORE), made when a channel in mode ziggurat is first attached.
    "zigbee": NET_CORE,
}

MANAGEMENT_TASKS = {
    "httpd": MGMT_CORE,
    "thread_rest": MGMT_CORE,
    "diag": MGMT_CORE,
    "c6_log": MGMT_CORE,
    "pcap_stream": MGMT_CORE,
    "ota_http": MGMT_CORE,
    "radio_fw": MGMT_CORE,
    "rom_recover": MGMT_CORE,
    "console_repl": MGMT_CORE,
    # Radio channel mux: the owner task and the USB host, started at every boot.
    "radio_mux": MGMT_CORE,
    "usb_lib": MGMT_CORE,
    "USB-CDC": MGMT_CORE,
    # RCP proxy (the rcp-proxy mode's sockets), started at every boot.
    "rcp_proxy": MGMT_CORE,
    # Zigbee mode: the WebSocket sender, made with the zigbee task.
    "zigbee_tx": MGMT_CORE,
}

# Present in every BR-mode boot with the jumper fitted; the others exist only while their job or request runs.
ALWAYS_PRESENT = {
    "tcpip",
    "emac_rx",
    "ot_main",
    "sys_evt",
    "mdns",
    "esp_timer",
    "httpd",
    "diag",
    "c6_log",
    "console_repl",
    "radio_mux",
    "usb_lib",
    "USB-CDC",
    "rcp_proxy",
}
