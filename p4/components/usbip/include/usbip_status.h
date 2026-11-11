// USB/IP status (spec section 9): every field of GET /device/usbip. The network side fills the device, client, state,
// counter and memory parts (usbip_net_snapshot); the settings module fills enabled, active, pending_reboot, port and
// iface.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "usbip_budget.h"
#include "usbip_core.h"
#include "usbip_engine.h"
#include "usbip_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool enabled;         // the stored flag
    bool active;          // USB/IP owns the port this boot
    bool pending_reboot;  // enabled != active
    usbip_state_t state;
    char reason[96];      // why blocked or unsupported; empty otherwise
    uint16_t port;
    const char *iface;    // "single" or "mgmt"
    bool has_device;      // an opened device (exportable or attached): dev and the strings are valid
    usbip_dev_rec_t dev;
    char manufacturer[64], product[64], serial[64];
    uint8_t endpoints;    // non-control endpoints over the interfaces' alternate setting 0
    bool has_client;      // a session runs: client and client_since are valid
    char client[48];      // "192.168.10.20:51322" or "[2001:db8::1]:51322"
    int64_t client_since; // Unix time (s) of the import
    usbip_counters_t counters;
    uint32_t sessions, refused;
    usbip_mem_stats_t mem;
    char last_error[96];  // empty when none
} usbip_status_t;

#ifdef __cplusplus
}
#endif
