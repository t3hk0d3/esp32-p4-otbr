#pragma once

// Raw link API: needed by the sniffer (spec 5.5) and on the RCP side.
#define OPENTHREAD_CONFIG_LINK_RAW_ENABLE 1

// This file is included before ESP-IDF's FTD defaults, so what is defined here wins.

// Not a Thread Test Harness reference device. With the flag on, the vendor name has to start with "RD:", and the
// Network Diagnostic reply would carry that. Turning it off also changes (checked against OpenThread v6.1 sources):
//  - the SRP client buffer limits fall to 2 host addresses (pinned below; the service pool is sized by ESP-IDF's
//    Kconfig CONFIG_OPENTHREAD_SRP_CLIENT_MAX_SERVICES, which does not depend on the flag);
//  - the TMF origin filter becomes unconditional;
//  - otThreadGetParentInfo needs the child role (unused here);
//  - the test build's OT CLI loses the test-harness commands (none is used by p4/pytest).
#define OPENTHREAD_CONFIG_REFERENCE_DEVICE_ENABLE 0
#define OPENTHREAD_CONFIG_SRP_CLIENT_BUFFERS_MAX_HOST_ADDRESSES 10

// Network Diagnostic vendor TLVs (spec: device info). The SW version is set at run time by br_core
// (otThreadSetVendorSwVersion), from the build stamp.
#define OPENTHREAD_CONFIG_NET_DIAG_VENDOR_NAME "ESP32-P4-WIFI6-POE-ETH"
#define OPENTHREAD_CONFIG_NET_DIAG_VENDOR_MODEL "OpenThread Border Router"
#define OPENTHREAD_CONFIG_NET_DIAG_VENDOR_APP_URL "https://github.com/t3hk0d3/esp32-p4-otbr"
#define OPENTHREAD_CONFIG_NET_DIAG_VENDOR_INFO_SET_API_ENABLE 1
