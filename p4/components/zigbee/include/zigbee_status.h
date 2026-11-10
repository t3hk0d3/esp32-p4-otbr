// The Zigbee status snapshot (zigbee_get_status). Apart from zigbee.h so that pure code (device_api's JSON, host
// tests) can use the type without esp_http_server.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "zigbee_spinel.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZIGBEE_CHANNEL_ID_MAX 48 // RADIO_MUX_ID_MAX
#define ZIGBEE_TEXT_MAX 96
#define ZIGBEE_ADDRESS_MAX 48

typedef enum {
    ZIGBEE_STATE_IDLE,         // no channel attached yet: none in mode ziggurat, or its radio is not there
    ZIGBEE_STATE_STARTING,     // the RCP's attach sequence runs (first attach, a RESET, the radio coming back)
    ZIGBEE_STATE_RUNNING,      // the radio is up for the stack
    ZIGBEE_STATE_NO_RADIO,     // the channel went away (GONE); the stack keeps running without a radio
    ZIGBEE_STATE_INCOMPATIBLE, // the RCP cannot serve ziggurat (protocol version, radio caps); `reason` says why
} zigbee_state_t;

#define ZIGBEE_ZLOE_PEERS_MAX 8

// The ZLOE satellite (spike).
typedef struct {
    char state[24];
    char reason[64]; // why it failed
    uint16_t nwk;
    uint8_t peer_count;
    struct {
        uint8_t eui64[8]; // wire order
        uint16_t nwk;
        bool live;
        uint8_t lqi;
        uint32_t last_rx_age_ms; // UINT32_MAX: it never sent a datagram
    } peers[ZIGBEE_ZLOE_PEERS_MAX];
} zigbee_satellite_status_t;

typedef struct {
    char channel[ZIGBEE_CHANNEL_ID_MAX]; // the channel id; "" when no channel is in mode ziggurat
    char firmware[ZIGBEE_TEXT_MAX];      // the mux's firmware record of the channel ("spinel <version> <chip>"); ""
    zigbee_state_t state;
    char reason[ZIGBEE_TEXT_MAX]; // why not running (incompatible, no radio, the mux's reason while idle); "" else
    bool has_client;
    char client_address[ZIGBEE_ADDRESS_MAX];
    int64_t client_since; // seconds since the epoch; 0 when the board clock was not set
    bool has_last_client;
    char last_client_address[ZIGBEE_ADDRESS_MAX];
    int64_t last_client_left; // seconds since the epoch; 0 when the board clock was not set
    bool has_radio_config;
    zigbee_radio_config_t radio; // the radio as the stack last configured it
    struct {
        uint32_t rx;           // frames received from the radio and handed to the stack
        uint32_t rx_dropped;   // frames the mux dropped on the channel (pool or event queue full)
        uint32_t tx;           // transmits the RCP accepted
        uint32_t tx_no_ack;    // transmit results: no ACK
        uint32_t tx_cca;       // channel access failure
        uint32_t tx_failed;    // failed or aborted, and transmits refused before reaching the RCP
        uint32_t energy_scans; // energy scans started
        uint32_t rcp_resets;   // RCP resets (the mux's RESET and the RCP's own)
        uint32_t frames_in;    // protocol frames from the client to the stack
        uint32_t frames_out;   // protocol frames sent to the client
    } counters;
    struct {
        size_t current, peak; // bytes of the stack's PSRAM heap in use
    } heap;
    zigbee_satellite_status_t satellite;
    bool stack_started; // ziggurat_start ran (once per boot)
    int64_t started;    // when it ran, seconds since the epoch; 0 when not started or the clock was not set
} zigbee_status_t;

#ifdef __cplusplus
}
#endif
