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

#ifdef __cplusplus
}
#endif
