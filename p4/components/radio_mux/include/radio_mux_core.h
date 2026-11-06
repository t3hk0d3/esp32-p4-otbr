// Decisions of the radio channel mux. Pure: no ESP-IDF calls.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// "usb-" + vid + "-" + pid + "-" + serial is cut to 47 characters; the buffer adds the NUL.
#define RADIO_MUX_ID_MAX 48
#define RADIO_MUX_CP210X_FLOW_LEN 16
#define RADIO_MUX_RULES_MAX 4
#define RADIO_MUX_PROXY_DEFAULT_PORT 6638
#define RADIO_MUX_PROXY_HOST_MAX 64

typedef enum {
    RADIO_MUX_MODE_UNAVAILABLE,
    RADIO_MUX_MODE_OFF,
    RADIO_MUX_MODE_OTBR,
    RADIO_MUX_MODE_ZIGGURAT,
    RADIO_MUX_MODE_RCP_PROXY,
    RADIO_MUX_MODE_INVALID,
} radio_mux_mode_t;

typedef enum {
    RADIO_MUX_FW_UNKNOWN,
    RADIO_MUX_FW_SPINEL,
    RADIO_MUX_FW_ZIGGURAT,
    RADIO_MUX_FW_ZNP,
    RADIO_MUX_FW_EZSP,
} radio_mux_fw_t;

// What a registered consumer declares about its mode. otbr is built in; off and rcp-proxy always apply.
typedef struct {
    radio_mux_mode_t mode;
    radio_mux_fw_t needs; // UNKNOWN = any firmware
    uint8_t limit;        // channels that may run the mode at once; 0 = no limit
    bool reboot_on_change;
} radio_mux_rule_t;

typedef struct {
    radio_mux_rule_t rules[RADIO_MUX_RULES_MAX];
    size_t n;
} radio_mux_rules_t;

typedef struct {
    const char *id;
    radio_mux_mode_t mode; // the stored mode
    bool present;
    bool supported;
    radio_mux_fw_t fw;
} radio_mux_chan_view_t;

typedef enum {
    RADIO_MUX_APPLY,
    RADIO_MUX_APPLY_REBOOT,
    RADIO_MUX_MOVE,
    RADIO_MUX_REFUSE_BAD_VALUE,
    RADIO_MUX_REFUSE_UNAVAILABLE,
    RADIO_MUX_REFUSE_NOT_AVAILABLE,
    RADIO_MUX_REFUSE_INCOMPATIBLE,
    RADIO_MUX_REFUSE_MODE_TAKEN,
    RADIO_MUX_REFUSE_HYBRID,
    // radio_mux_set only, never from radio_mux_check_set_mode:
    RADIO_MUX_REFUSE_UNKNOWN_CHANNEL, // no channel with that id (404)
    RADIO_MUX_REFUSE_FAILED,          // the mux could not store or apply the change (not started, NVS, timeout: 500)
} radio_mux_verdict_t;

typedef enum {
    RADIO_MUX_PROXY_LISTEN,
    RADIO_MUX_PROXY_CONNECT,
} radio_mux_proxy_dir_t;

typedef enum {
    RADIO_MUX_PROXY_TCP,
    RADIO_MUX_PROXY_UDP,
} radio_mux_proxy_adapter_t;

typedef enum {
    RADIO_MUX_PROXY_HDLC,
    RADIO_MUX_PROXY_RAW,
} radio_mux_proxy_framing_t;

// The stored settings of the rcp-proxy mode. rcp_proxy depends on radio_mux, never the reverse.
typedef struct {
    radio_mux_proxy_dir_t dir;
    radio_mux_proxy_adapter_t adapter;
    uint16_t port;
    char host[RADIO_MUX_PROXY_HOST_MAX];
    radio_mux_proxy_framing_t framing;
} radio_mux_proxy_settings_t;

// Bits of radio_mux_set_req_t.proxy_fields: which rcp_proxy settings a change gives.
#define RADIO_MUX_PROXY_F_DIR (1u << 0)
#define RADIO_MUX_PROXY_F_ADAPTER (1u << 1)
#define RADIO_MUX_PROXY_F_PORT (1u << 2)
#define RADIO_MUX_PROXY_F_HOST (1u << 3)
#define RADIO_MUX_PROXY_F_FRAMING (1u << 4)

// The state of a channel within its mode (spec section 3, Channels).
typedef enum {
    RADIO_MUX_STATE_IDLE,     // mode off, or not attached
    RADIO_MUX_STATE_STARTING, // detection or reopen in progress
    RADIO_MUX_STATE_RUNNING,  // the mode's consumer is attached
    RADIO_MUX_STATE_FAULT,    // with a reason
    RADIO_MUX_STATE_BUSY,     // a C6 job holds the radio
} radio_mux_state_t;

typedef enum {
    RADIO_MUX_FW_SRC_NONE,    // no record yet
    RADIO_MUX_FW_SRC_PROBE,   // firmware detection at plug-in
    RADIO_MUX_FW_SRC_PASSIVE, // reported by a running consumer (radio_mux_note_firmware)
} radio_mux_fw_source_t;

// Per-channel counters. drops: frames (raw: chunks) lost because the pool, the event queue or the raw ring was full;
// rx_errors: HDLC frames with a bad CRC or too long for a slot.
typedef struct {
    uint32_t frames_in, frames_out;
    uint64_t bytes_in, bytes_out;
    uint32_t drops, resets, rx_errors;
} radio_mux_counters_t;

typedef enum {
    RADIO_MUX_POSTED,  // the event is in the queue: the consumer owns the slot
    RADIO_MUX_DROPPED, // the queue was full: the caller puts the slot back into the pool
} radio_mux_post_t;

// The attach state machine of one transport (spec section 3, Frames, events and copies: one attach path for boot,
// plug-in, return after GONE and mode change). The board code runs the action, then feeds the event it leads to.
typedef enum {
    RADIO_MUX_AS_ABSENT,    // no device
    RADIO_MUX_AS_PROBING,   // open in mode off, firmware detection running
    RADIO_MUX_AS_REOPENING, // detection over: close and reopen
    RADIO_MUX_AS_IDLE,      // open (or failed to open), no consumer attached
    RADIO_MUX_AS_ATTACHED,  // the stored mode's consumer is attached
    RADIO_MUX_AS_CLOSING,   // the device is being closed (unplug)
} radio_mux_attach_state_t;

typedef enum {
    RADIO_MUX_AEV_PLUG,          // a supported device appeared
    RADIO_MUX_AEV_DETECT_DONE,   // the detection gave its record
    RADIO_MUX_AEV_REOPENED,      // the reopen after detection finished (whether it opened or not)
    RADIO_MUX_AEV_MODE_SET,      // the stored mode changed
    RADIO_MUX_AEV_UNPLUG,        // the device went away
    RADIO_MUX_AEV_CLOSED,        // the close the machine asked for is done
    RADIO_MUX_AEV_ATTACH_FAILED, // the consumer's attach returned an error
} radio_mux_attach_ev_t;

typedef enum {
    RADIO_MUX_ACT_NONE,
    RADIO_MUX_ACT_PROBE,      // open the device in mode off and start detection
    RADIO_MUX_ACT_REOPEN,     // close and reopen the device at the channel's line settings
    RADIO_MUX_ACT_ATTACH,     // attach the stored mode's consumer
    RADIO_MUX_ACT_DETACH,     // post GONE and detach the consumer; the device stays open
    RADIO_MUX_ACT_REATTACH,   // post GONE, detach the old consumer, attach the stored mode's consumer
    RADIO_MUX_ACT_CLOSE,      // close the device (no consumer attached), then feed CLOSED
    RADIO_MUX_ACT_GONE_CLOSE, // post GONE, detach the consumer, close the device, then feed CLOSED
    RADIO_MUX_ACT_DEFER,      // keep the new device: it is probed once the old one is CLOSED
} radio_mux_attach_act_t;

typedef struct {
    radio_mux_attach_state_t state;
    bool plug_pending; // a PLUG came while CLOSING
} radio_mux_attach_t;

typedef enum {
    RADIO_MUX_DRV_NONE,
    RADIO_MUX_DRV_CP210X,
    RADIO_MUX_DRV_CH34X,
    RADIO_MUX_DRV_CDC_ACM,
} radio_mux_driver_t;

typedef enum {
    RADIO_MUX_BAUD_EXACT,
    RADIO_MUX_BAUD_CLOSE,
    RADIO_MUX_BAUD_FAR,
} radio_mux_baud_t;

// "c6" is the on-board channel. A USB channel is "usb-<vid>-<pid>", plus "-<serial>" when the device reports a serial
// string: lowercase 4-digit hex, serial reduced to [A-Za-z0-9], the whole id at most 47 characters. serial may be NULL.
void radio_mux_channel_id(uint16_t vid, uint16_t pid, const char *serial, char out[RADIO_MUX_ID_MAX]);

// Wire names: "unavailable", "off", "otbr", "ziggurat", "rcp-proxy". Anything else (NULL included) is INVALID.
radio_mux_mode_t radio_mux_mode_parse(const char *name);
// NULL for INVALID.
const char *radio_mux_mode_name(radio_mux_mode_t mode);
// "unknown", "spinel", "ziggurat", "znp", "ezsp".
const char *radio_mux_fw_name(radio_mux_fw_t fw);

// Whether firmware of this type can run the mode. otbr needs spinel; off and rcp-proxy fit anything; a registered mode
// uses the type its registration declares; any other mode fits nothing.
bool radio_mux_fits(const radio_mux_rules_t *rules, radio_mux_mode_t mode, radio_mux_fw_t fw);

// Decision for setting channel `target` (index into chans[0..n)) to `want`. On RADIO_MUX_MOVE, *move_from is the index
// of the channel that currently has otbr; move_from may be NULL. Checks run in the order of the verdicts: bad value,
// unavailable channel, mode not registered, firmware fit, hybrid, move or limit, reboot.
radio_mux_verdict_t radio_mux_check_set_mode(const radio_mux_rules_t *rules, const radio_mux_chan_view_t *chans,
                                             size_t n, size_t target, radio_mux_mode_t want, size_t *move_from);

// The mode a channel takes once its firmware is detected: the stored mode when it fits, else OFF with the reason in
// `reason` (empty when the stored mode is kept). reason may be NULL.
radio_mux_mode_t radio_mux_after_detect(const radio_mux_rules_t *rules, radio_mux_mode_t stored, radio_mux_fw_t fw,
                                        char *reason, size_t reason_cap);

void radio_mux_proxy_defaults(radio_mux_proxy_settings_t *out);

// VID/PID tables (CP210x, CH34x) first, then the CDC rule: an interface of class 0x0A plus either the device class
// or an interface of class 0x02. Anything else is NONE.
radio_mux_driver_t radio_mux_pick_driver(uint16_t vid, uint16_t pid, uint8_t dev_class,
                                         const uint8_t *intf_classes, size_t n_intf);

// "cp210x", "ch34x", "cdc_acm"; NULL for NONE.
const char *radio_mux_driver_name(radio_mux_driver_t d);

// Accepted line speeds: 38400, 57600, 115200, 230400, 460800, 921600, 1000000, 2000000.
bool radio_mux_baud_ok(uint32_t baud);

// EXACT when equal, CLOSE within 3 % (integer arithmetic), FAR otherwise (actual 0 included).
radio_mux_baud_t radio_mux_baud_verdict(uint32_t requested, uint32_t actual);

// CP210x SET_FLOW payload (CP210X_SERIAL_STATUS layout), four little-endian uint32: ulControlHandshake,
// ulFlowReplace, ulXonLimit, ulXoffLimit.
void radio_mux_cp210x_flow(bool on, uint8_t out[RADIO_MUX_CP210X_FLOW_LEN]);

// One step of the attach state machine. attach_wanted is radio_mux_attach_wanted for the channel as it is now; only
// REOPENED and MODE_SET read it. Events that do not apply to the state change nothing and return NONE; a PLUG while a
// device is open is ignored (one device per transport).
radio_mux_attach_act_t radio_mux_attach_next(radio_mux_attach_t *a, radio_mux_attach_ev_t ev, bool attach_wanted);

// Whether the consumer of the channel's stored mode attaches (R13): the channel is present and supported, a consumer
// is registered for the stored mode (never for off or unavailable) and the detected firmware fits it.
bool radio_mux_attach_wanted(const radio_mux_rules_t *rules, const radio_mux_chan_view_t *ch,
                             bool consumer_registered);

// Accounting of one RX_FRAME post that was tried without waiting (timeout 0): POSTED counts the frame and its bytes
// in; DROPPED (queue_full) counts a drop and tells the caller to put the slot back. Never waits.
radio_mux_post_t radio_mux_post_rx(bool queue_full, size_t len, radio_mux_counters_t *c);

// NVS key of a channel's settings blob: "c6" for the C6, else "u" + 8 lowercase hex digits of the FNV-1a (32 bit)
// hash of the id.
void radio_mux_nvs_key(const char *id, char out[16]);

// "idle", "starting", "running", "fault", "busy"; NULL for anything else.
const char *radio_mux_state_name(radio_mux_state_t state);
// "probe", "passive"; NULL for NONE.
const char *radio_mux_fw_source_name(radio_mux_fw_source_t src);

// Stored rcp_proxy settings: known direction, adapter and framing; port 1..65535 except 80 and 179 (the board's own
// listeners); connect needs a host.
bool radio_mux_proxy_ok(const radio_mux_proxy_settings_t *s);

#ifdef __cplusplus
}
#endif
