"""Expected core of each task, mirroring the placement table of spec 5.11 (one source for the scheduling tests).

Names are the FreeRTOS task names as reported in /device/diag tasks[].name:
- tcpip: lwIP (TCPIP_THREAD_NAME, CONFIG_LWIP_TCPIP_TASK_AFFINITY_CPU0)
- emac_rx: esp_eth EMAC RX task (ETH_MAC_FLAG_PIN_TO_CORE, driver installed from the core-0 net_init task)
- ot_main: OpenThread task (br_core, CONFIG_OPENTHREAD_TASK_NAME)
- sys_evt: default esp_event loop (task_core_id 0 in IDF)
- mdns: mDNS service task (CONFIG_MDNS_TASK_AFFINITY_CPU0)
- esp_timer: esp_timer task, always core 0
- httpd: esp_http_server (httpd_config_t.core_id, patch 1); thread_rest: upstream REST worker (patch 11)
- diag, c6_log, pcap_stream, ota_http, radio_fw, rom_recover, radio_mux, radio_mux_c6, rcp_proxy, rcp_resolve, br_radio:
  our tasks, pinned in code
- usb_lib, USB-CDC: the USB host daemon and the CDC driver task, pinned to NET_CORE by radio_mux
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
    # Radio channel mux: the C6 transport task (SPI2), started at every boot.
    "radio_mux_c6": NET_CORE,
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
    "usb_lib": NET_CORE,  # the USB host daemon; it installs the host, so the USB interrupt is on NET_CORE too
    "USB-CDC": NET_CORE,
    # RCP proxy (the rcp-proxy mode's sockets) and its name lookups, started at every boot.
    "rcp_proxy": MGMT_CORE,
    "rcp_resolve": MGMT_CORE,
    # br_core's radio task: OpenThread on the OTBR channel (start, stop, moves), started at every boot.
    "br_radio": MGMT_CORE,
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
    "radio_mux_c6",
    "usb_lib",
    "USB-CDC",
    "rcp_proxy",
    "rcp_resolve",
    "br_radio",
}
