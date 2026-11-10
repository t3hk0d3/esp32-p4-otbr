// The ZLOE satellite's bootstrap (spike, docs/superpowers/plans/2026-11-10-zloe-satellite.md): a local client of the
// stack's control protocol, in place of the WebSocket's. It waits for a clock, fetches the network settings from a
// coordinator on the backbone, configures the stack as a router, announces it and answers the ZDP requests of its
// interview, as ziggurat-server's satellite.rs does. Pure; every call is made on the zigbee task.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ZIGBEE_SATELLITE_REASON_MAX 64
#define ZIGBEE_SATELLITE_CLOCK_TIMEOUT_MS 60000
#define ZIGBEE_SATELLITE_COORDINATOR_TIMEOUT_MS 60000
#define ZIGBEE_SATELLITE_REQUEST_TIMEOUT_MS 10000
#define ZIGBEE_SATELLITE_NETINFO_PERIOD_MS 1000
// channel .. aps_frame_counter of NetworkState on the wire.
#define ZIGBEE_SATELLITE_STATE_LEN 82

typedef enum {
    ZIGBEE_SATELLITE_IDLE,
    ZIGBEE_SATELLITE_STARTING, // asked the stack for its EUI64
    ZIGBEE_SATELLITE_WAITING_CLOCK,
    ZIGBEE_SATELLITE_WAITING_COORDINATOR,
    ZIGBEE_SATELLITE_CONFIGURING, // Configure, then StartNetwork
    ZIGBEE_SATELLITE_RUNNING,
    ZIGBEE_SATELLITE_FAILED, // `reason` says why; a reboot tries again
} zigbee_satellite_state_t;

typedef struct {
    int64_t (*now_ms)(void *ctx);
    // The wall clock in seconds since the epoch. false: not set.
    bool (*clock_unix)(void *ctx, int64_t *out);
    // A control-protocol frame into the stack.
    void (*host_frame)(void *ctx, const uint8_t *frame, size_t len);
    // Ask the first coordinator discovery found for the settings. false: none known yet.
    bool (*request_netinfo)(void *ctx);
    // The network's extended PAN ID (wire order), before Configure.
    void (*set_epan)(void *ctx, const uint8_t epan[8]);
    // The stack runs: announce this node on the backbone. EUI64s in wire order.
    void (*announce)(void *ctx, const uint8_t eui64[8], const uint8_t epan[8], uint16_t nwk);
} zigbee_satellite_ops_t;

typedef struct {
    const zigbee_satellite_ops_t *ops;
    void *ctx;
    zigbee_satellite_state_t state;
    char reason[ZIGBEE_SATELLITE_REASON_MAX];
    uint16_t request_id;
    uint8_t pending; // the command whose response we wait for; 0xFF: none
    int64_t deadline_ms;
    int64_t next_netinfo_ms;
    uint8_t eui64[8];
    uint8_t epan[8];
    uint16_t nwk;
} zigbee_satellite_t;

void zigbee_satellite_init(zigbee_satellite_t *s, const zigbee_satellite_ops_t *ops, void *ctx);
// The stack just started.
void zigbee_satellite_start(zigbee_satellite_t *s);
// Periodically: the clock and coordinator waits, and the timeouts.
void zigbee_satellite_on_tick(zigbee_satellite_t *s);
// A control-protocol frame out of the stack.
void zigbee_satellite_on_frame(zigbee_satellite_t *s, const uint8_t *frame, size_t len);
// A coordinator's settings: an encoded NetworkInfoPayload.
void zigbee_satellite_on_netinfo(zigbee_satellite_t *s, const uint8_t *data, size_t len);

const char *zigbee_satellite_state_name(zigbee_satellite_state_t state);
// A stable address from the EUI64 (wire order), outside 0x0000 and 0xFFF8 and above.
uint16_t zigbee_satellite_derive_nwk(const uint8_t eui64[8]);
// The frame-counter seed: seconds since 2026-01-01, times 10 (10 frames/s of headroom across restarts).
uint32_t zigbee_satellite_clock_counter(int64_t unix_seconds);

#ifdef __cplusplus
}
#endif
