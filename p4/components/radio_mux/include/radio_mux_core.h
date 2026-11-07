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
// The id of the on-board C6 channel (radio_mux.h documents it with the API).
#define RADIO_MUX_C6_ID "c6"
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

// One bit per mode, for a set of modes (radio_mux_available_modes).
#define RADIO_MUX_MODE_BIT(mode) (1u << (unsigned)(mode))

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
    RADIO_MUX_REFUSE_PROBE_FAILED,    // a move of otbr: the target's RCP probe failed, nothing changed (409 probe_failed)
    RADIO_MUX_REFUSE_BUSY,            // the channel or OpenThread is busy (a move or a radio job runs): try again (409)
    RADIO_MUX_REFUSE_PENDING,         // posted, but the owner did not answer in time: the change may still be applied (503)
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
// rx_errors: HDLC frames with a bad CRC or too long for a slot; uart_errors: receive errors the USB bridge itself
// reported (CH34x status packets with an overrun, parity, framing or receive error bit, radio_mux_ch34x_uart_error).
typedef struct {
    uint32_t frames_in, frames_out;
    uint64_t bytes_in, bytes_out;
    uint32_t drops, resets, rx_errors, uart_errors;
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

// The modes the API offers: off always, plus every mode a rule registers (otbr, ziggurat, rcp-proxy). A set of
// RADIO_MUX_MODE_BIT; never unavailable or invalid.
uint32_t radio_mux_available_modes(const radio_mux_rules_t *rules);

// Whether firmware of this type can run the mode. otbr needs spinel; off and rcp-proxy fit anything; a registered mode
// uses the type its registration declares; any other mode fits nothing.
bool radio_mux_fits(const radio_mux_rules_t *rules, radio_mux_mode_t mode, radio_mux_fw_t fw);

// Decision for setting channel `target` (index into chans[0..n)) to `want`. RADIO_MUX_MOVE is every change of where
// OpenThread runs, each run by br_core as a long operation: otbr while another channel has it (a move; *move_from is
// that channel's index), otbr while no channel has it (a start; *move_from is n), and the otbr channel leaving otbr
// for another mode without a reboot (a stop; *move_from is target). move_from may be NULL. Checks run in the order of
// the verdicts: bad value, unavailable channel, mode not registered, firmware fit, hybrid (otbr next to a stored
// ziggurat only while a ziggurat consumer is registered), move or limit, start, reboot, stop.
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

// What the consumer of a port gets: the C6 port always delivers and takes spinel frames (its SPI transport has no byte
// stream), whatever the consumer declares; a byte transport (USB) follows the consumer's `frames`.
bool radio_mux_port_frames(bool c6_port, bool consumer_frames);

// How long a USB write of n bytes may take: RADIO_MUX_TX_BASE_MS plus twice its time on the wire (10 bits a byte) at
// `baud`, rounded up. A baud of 0 (unknown) counts as the slowest accepted one, 38400.
#define RADIO_MUX_TX_BASE_MS 100
uint32_t radio_mux_tx_timeout_ms(size_t n, uint32_t baud);

// A CH34x status packet (the bridge's interrupt IN endpoint, e.g. `20 ff 0f aa`): byte 0 carries the UART state, whose
// transient bits (the ch34x driver's CH34X_UART_STATE_TRANSIENT_MASK: overrun, parity, framing, receive error) mean
// one UART error (as the driver's defines read it; not confirmed against a datasheet). Packets shorter than RADIO_MUX_CH34X_STATUS_LEN are not status packets.
#define RADIO_MUX_CH34X_STATUS_LEN 4
#define RADIO_MUX_CH34X_UART_ERR_MASK 0x07
// Whether the packet reports a UART error: false for no error bit, a short packet or NULL.
bool radio_mux_ch34x_uart_error(const uint8_t *pkt, size_t len);

// A USB dongle's reset is a close and reopen. One that gets nothing back is not reset forever: the
// RADIO_MUX_USB_DEAD_RESETS-th reset request in a row with nothing received in between (no RX frame, no raw byte) is
// not carried out; the channel goes to fault with RADIO_MUX_USB_DEAD_REASON and its consumer gets GONE. Unplug and
// replug, or a PUT of its mode, opens it again.
#define RADIO_MUX_USB_DEAD_RESETS 3
#define RADIO_MUX_USB_DEAD_REASON "no answer after 3 resets"

typedef struct {
    uint8_t unanswered; // reset requests since the last thing received
} radio_mux_reset_count_t;

// A reset request: counts it. True when it is the RADIO_MUX_USB_DEAD_RESETS-th in a row (give up, no reopen; the count
// starts again from 0), false when the device is to be reopened.
bool radio_mux_reset_count_request(radio_mux_reset_count_t *c);
// Something was received from the device, or a new attach starts: the count starts again from 0.
void radio_mux_reset_count_clear(radio_mux_reset_count_t *c);

// "idle", "starting", "running", "fault", "busy"; NULL for anything else.
const char *radio_mux_state_name(radio_mux_state_t state);
// "probe", "passive"; NULL for NONE.
const char *radio_mux_fw_source_name(radio_mux_fw_source_t src);

typedef enum {
    RADIO_MUX_PROXY_CHECK_OK,
    RADIO_MUX_PROXY_CHECK_BAD_KIND,  // an unknown direction, adapter or framing
    RADIO_MUX_PROXY_CHECK_BAD_PORT,  // 0, 80 or 179 (the board's own listeners)
    RADIO_MUX_PROXY_CHECK_NEED_HOST, // connect without a host
    RADIO_MUX_PROXY_CHECK_BAD_HOST,  // not an IPv4 or IPv6 literal, nor a DNS or .local name of at most 63 characters
} radio_mux_proxy_check_t;

// The one check of rcp_proxy settings (a PUT, the stored settings at load, rcp_proxy at attach): known direction,
// adapter and framing, then the port (1..65535 except 80 and 179), then a host for connect, then the host's syntax
// when one is given (in either direction: a host kept for a later switch to connect is valid). The framing is not
// checked against the adapter: it is kept with tcp for a later switch to udp.
radio_mux_proxy_check_t radio_mux_proxy_check(const radio_mux_proxy_settings_t *s);

// The settings a change leaves: `stored` with the fields of `given` that `fields` (RADIO_MUX_PROXY_F_* bits) names
// over it, into *out; then radio_mux_proxy_check on them. No field given: *out is `stored` and the result
// RADIO_MUX_PROXY_CHECK_OK (a change without rcp_proxy settings never fails on settings stored earlier).
radio_mux_proxy_check_t radio_mux_proxy_merge(const radio_mux_proxy_settings_t *stored, uint8_t fields,
                                              const radio_mux_proxy_settings_t *given, radio_mux_proxy_settings_t *out);

/* ---- the C6 over SPI (OpenThread's spi_frame.hpp) ---- */

// Every SPI transaction starts with a 5-byte header in both directions: the flag byte (pattern 0x02 in its low two
// bits, the reset bit 0x80), then the accept length and the data length, both uint16 little-endian.
#define RADIO_MUX_SPI_HDR_LEN 5
#define RADIO_MUX_SPI_FLAG_RESET 0x80
#define RADIO_MUX_SPI_FLAG_PATTERN 0x02
#define RADIO_MUX_SPI_PATTERN_MASK 0x03
// Accept length of a transaction that carries a frame of ours (as ESP-IDF's spinel SPI interface does): the frame's
// length, at least this many bytes.
#define RADIO_MUX_SPI_SMALL_ACCEPT 32
// Transactions are clocked in whole cache lines: the receive buffer is DMA'd into in place, without a bounce buffer.
#define RADIO_MUX_SPI_XFER_ALIGN 64

typedef struct {
    bool reset;          // the sender (re)started since its previous transaction
    uint16_t accept_len; // bytes of data the sender takes in this transaction
    uint16_t data_len;   // bytes of data the sender puts after the header
} radio_mux_spi_hdr_t;

void radio_mux_spi_hdr_encode(const radio_mux_spi_hdr_t *h, uint8_t out[RADIO_MUX_SPI_HDR_LEN]);
// false when the flag byte's pattern bits are not 0x02 (the RCP not ready: a floating or held MISO reads 0xFF or 0x00);
// *out is then left as it was.
bool radio_mux_spi_hdr_decode(const uint8_t in[RADIO_MUX_SPI_HDR_LEN], radio_mux_spi_hdr_t *out);

// What one full-duplex transaction gave, from the RCP's header (rx_hdr), the data length we sent (tx_len) and the
// accept length we sent (accept).
typedef struct {
    bool valid;     // pattern right and both lengths at most `frame_max`; nothing else is set when false
    bool rcp_reset; // the RCP's reset bit
    uint16_t rx_len; // the RCP sent a frame of this many bytes after the header; 0 = none (or more than we accepted)
    bool tx_taken;   // tx_len was 0, or the RCP accepted that much
} radio_mux_spi_result_t;

void radio_mux_spi_check(const uint8_t rx_hdr[RADIO_MUX_SPI_HDR_LEN], uint16_t tx_len, uint16_t accept,
                         uint16_t frame_max, radio_mux_spi_result_t *out);

// Bytes to clock for a transaction: the header plus the larger of tx_len and accept, rounded up to
// RADIO_MUX_SPI_XFER_ALIGN.
size_t radio_mux_spi_xfer_len(uint16_t tx_len, uint16_t accept);

// The C6's SPI slave re-arms after each transaction: its done callback raises INT, then queues the next transaction.
// Clocking again before that sees an unloaded slave (garbage headers, a misaligned stream). How long to wait before
// the next transaction: 0 when at least gap_us passed since the previous one ended at last_end_us (negative: none
// yet), else the rest of the gap. A clock that went backwards counts as "just ended" (the whole gap).
uint32_t radio_mux_spi_gap_wait_us(int64_t now_us, int64_t last_end_us, uint32_t gap_us);

// What rcp_sync's last probe of the C6 said (the fields of rcp_sync_probe_result_t the mux reads). All false: no probe
// ran yet.
typedef struct {
    bool updater_reachable;
    bool installed_valid;
    bool esp_hosted;
    bool rom_recovery_failed;
} radio_mux_c6_probe_t;

// Whether c6 is listed present: not when the C6 runs the factory ESP-Hosted firmware, when a ROM recovery failed and
// the updater stayed silent, or when the updater answered with no RCP installed; `reason` (may be NULL) then names
// which, else it is empty. No probe yet, or a silent updater without a failed recovery: present (the installed
// firmware is tried, as br_core does).
bool radio_mux_c6_present(const radio_mux_c6_probe_t *probe, char *reason, size_t cap);

// Whether a C6 job (push, recovery, probe) is refused: only while the C6's stored mode is rcp-proxy, its consumer is
// attached and it reported a connected peer.
bool radio_mux_c6_job_refused(radio_mux_mode_t stored_mode, bool attached, bool has_peer);

/* ---- attach handles ---- */

// A consumer's handle (radio_mux_chan_t *, radio_mux.h) is one attach or lease, not the channel: the pointer value
// packs the channel's slot (low RADIO_MUX_HANDLE_SLOT_BITS bits) and the generation of that attach (the rest). It is
// never dereferenced. Every attach and lease takes the next generation; a transport accepts calls only through the
// handle of its current generation.
#define RADIO_MUX_HANDLE_SLOT_BITS 3
#define RADIO_MUX_HANDLE_SLOTS (1u << RADIO_MUX_HANDLE_SLOT_BITS)
// The largest generation that fits a 32-bit pointer next to the slot.
#define RADIO_MUX_GEN_MAX (UINT32_MAX >> RADIO_MUX_HANDLE_SLOT_BITS)

// The generation after `gen`: gen + 1, from RADIO_MUX_GEN_MAX (or anything above it) back to 1; never 0, which no
// handle carries (0 = no attach yet).
uint32_t radio_mux_gen_next(uint32_t gen);
// The handle value of slot `slot` (< RADIO_MUX_HANDLE_SLOTS) at generation `gen` (1..RADIO_MUX_GEN_MAX); 0 when either
// is out of range. Never 0 otherwise.
uintptr_t radio_mux_handle_pack(size_t slot, uint32_t gen);
// The slot and generation of handle value `h`; false (nothing set) for 0 or a generation of 0.
bool radio_mux_handle_unpack(uintptr_t h, size_t *slot, uint32_t *gen);
// Whether a handle of generation `gen` may use a transport whose current attach has generation `current`: equal, and
// not 0 (nothing attached to the transport yet).
bool radio_mux_handle_current(uint32_t gen, uint32_t current);

#ifdef __cplusplus
}
#endif
