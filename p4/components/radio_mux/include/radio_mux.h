// Radio channel mux: every radio (the on-board C6, a USB dongle) is a channel with a stored mode, and the channel/mode
// orchestrator binds the mode's instance to it while the channel is ready and its firmware fits (design
// docs/superpowers/specs/2026-11-08-channel-mode-orchestrator-design.md). A published contract: OpenThread (otbr),
// rcp_proxy (rcp-proxy, one instance per channel) and the Zigbee work (ziggurat) are the instances; the instance bound
// to a channel is its consumer (it takes its frames and sends through its handle).
//
// Threading: one owner task opens and closes every device and delivers the instances' inputs (bind, unbind, reinit,
// lost, back). The transports never call into an instance: they post events (a FreeRTOS queue the instance binds, or
// the channel's own queue signalled through an eventfd). Unless a function says otherwise it may be called from any
// task, never from an ISR.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "radio_mux_core.h"
#include "radio_mux_detect.h"
#include "radio_mux_orch_core.h"

#ifdef __cplusplus
extern "C" {
#endif

// Channels the mux knows at once: c6 plus up to 7 USB channels (attached or remembered in NVS). Only one USB device
// is open at a time. When an 8th USB channel appears, the unplugged one in mode off seen longest ago is forgotten (one
// stored in another mode never is, and a device with no driver never makes room); with none, the new one is not listed.
#define RADIO_MUX_CHANNELS_MAX 8
// Eventfds the mux creates: one per channel it can have open at once (c6, one USB). app_main adds them to the eventfd
// VFS registration.
#define RADIO_MUX_EVENTFD_COUNT 2
// Frame-pool slots per transport. A consumer's bound queue should hold at least this many events plus 2, so that
// GONE and RESET always fit behind the frames it has not taken yet.
#define RADIO_MUX_POOL_SLOTS 8
// Largest spinel frame a channel carries.
#define RADIO_MUX_FRAME_MAX 2048
#define RADIO_MUX_LABEL_MAX 48
#define RADIO_MUX_SERIAL_MAX 32
#define RADIO_MUX_PEER_MAX 64
// The rcp-proxy's last open or connect failure, as text (rcp_proxy fills it, device_api renders it).
#define RADIO_MUX_PROXY_ERROR_MAX 48
// RADIO_MUX_BUILTIN_ID ("builtin-c6", radio_mux_core.h): the id of the built-in RCP's channel, kind
// RADIO_MUX_KIND_BUILTIN; USB channels are "usb-<vid>-<pid>[-serial]".

/* ---- events ---- */

typedef enum {
    RADIO_MUX_EV_RX_FRAME, // slot: the first byte (spinel header) of one spinel frame, len its length; release it
    RADIO_MUX_EV_RESET,    // the mux reset or reopened the radio (radio_mux_reset); slot NULL
    RADIO_MUX_EV_GONE,     // the channel is no longer yours (unplugged, failed, or its mode changed); slot NULL
} radio_mux_ev_type_t;

typedef struct {
    radio_mux_ev_type_t type;
    uint8_t *slot;
    size_t len;
    int64_t rx_us; // esp_timer time the transport received the frame (RX_FRAME), else the post time
} radio_mux_ev_t;

// An instance refused a reinit because a peer is connected to it (the RCP proxy): 409 c6_busy.
#define RADIO_MUX_ERR_PEER_CONNECTED 0x7c01

/* ---- handles ---- */

// A consumer's handle: one attach (or lease) of a channel, not the channel itself. The mux passes a new handle to every
// attach (and returns one from every radio_mux_lease); detach gets the handle its attach got. The handle can be passed
// to every call below at any time (it is never freed and never dereferenced by the consumer), but once the transport
// has a newer attach or lease, the calls through it are refused: radio_mux_poll and radio_mux_chan_frames false,
// radio_mux_read_raw 0, radio_mux_tx and radio_mux_bind_queue ESP_ERR_INVALID_STATE, radio_mux_report,
// radio_mux_reset and radio_mux_note_firmware ignored. radio_mux_release keeps working (a slot of an earlier attach
// goes back to its pool), as do radio_mux_chan_id, radio_mux_eventfd and radio_mux_get_proxy (they name the channel).
//
// For a consumer: keep the handle attach gave you and use only it until your detach; compare handles to tell your
// attaches apart, and use radio_mux_chan_id (not the handle) to tell channels apart. Since the generations: a handle
// no longer stays valid for the life of the firmware, two attaches of the same channel get different handles, and a
// handle from an earlier attach can no longer take events, raw bytes or the transmit path of the attach after it (it
// used to work until the next attach on the transport; it still does).
// (radio_mux_chan_t is declared in radio_mux_orch_core.h, with the input that carries it.)

/* ---- mode instances (the orchestrator binds them to channels) ---- */

// radio_mux_input_t, one input from the orchestrator to an instance: radio_mux_orch_core.h.

typedef struct {
    radio_mux_mode_t mode;  // otbr, ziggurat or rcp-proxy
    radio_mux_fw_t needs;   // firmware the mode needs; UNKNOWN = any (otbr always needs spinel, rcp-proxy nothing)
    uint8_t limit;          // channels that may run the mode at once; 0 = no limit (otbr is always 1)
    bool reboot_on_change;  // binding or unbinding it reboots the board (the orchestrator answers APPLY_REBOOT)
    // Byte transports (USB) only. true: spinel frames (RX_FRAME events, radio_mux_tx takes frames); false: raw bytes
    // (radio_mux_read_raw, radio_mux_tx takes bytes). Ignored on the C6, which always delivers and takes frames: ask
    // radio_mux_chan_frames once bound.
    bool frames;
    bool per_channel;       // one instance per channel bound to the mode (the RCP proxy); else a singleton
    // Called on the owner task for every input. Copy *in as the latest input and wake the instance's own task: never
    // block, and call nothing of the mux that posts to the owner task or waits for it (reading the channel through the
    // handle is fine: radio_mux_chan_id, radio_mux_chan_frames, radio_mux_get_proxy). The instance acts on the latest
    // input from its task and ignores one whose seq is older than one it already handled.
    void (*input)(const radio_mux_input_t *in, void *ctx);
    void *ctx;
} radio_mux_instance_t;

// Before radio_mux_start only. The declaration is copied. ESP_ERR_INVALID_STATE for a duplicate mode or after the
// start, ESP_ERR_INVALID_ARG for off, unavailable or an unknown mode or without `input`, ESP_ERR_NO_MEM when
// RADIO_MUX_RULES_MAX instances are registered.
esp_err_t radio_mux_register_instance(const radio_mux_instance_t *inst);

// The instance's own state and reason on the channel of handle `ch`, shown in the read API (mode_state, mode_reason;
// state at most 15 characters). Ignored unless `ch` is the transport's current handle. Copies under the registry's
// lock; never waits for the owner task. reason may be NULL ("").
void radio_mux_report(radio_mux_chan_t *ch, const char *state, const char *reason);

// The answer to the REINIT input `seq` received with handle `ch`: ESP_OK accepts (the instance let go of the radio),
// anything else refuses with that code and `reason` (may be NULL; cut to 71 characters). Answer when you let go: at
// once, or once a drain ends; no answer within RADIO_MUX_REINIT_REPLY_MS is a refusal. Ignored for a stale handle or
// seq. Never blocks and is never lost: the answer waits in a mailbox the owner drains (ESP_OK).
esp_err_t radio_mux_reinit_reply(radio_mux_chan_t *ch, uint32_t seq, esp_err_t verdict, const char *reason);

/* ---- the built-in RCP (the board's own radio: the C6 on this board) ---- */

// A board's built-in RCP as its implementation (the builtin_rcp component) describes it: the channel's id, label and
// chip, and the maintenance ops. The ops run on the caller's task while the channel is in reinit (until maintenance is
// the reinit state: inside a C6 job), never on the owner task, except `present`, which never blocks.
typedef struct {
    const char *id;    // the channel's id
    const char *label; // the channel's label
    const char *chip;  // e.g. "esp32c6"
    // Whether the RCP is usable after its last probe; otherwise *kind (RADIO_MUX_FAULT_FIRMWARE) and why in `reason`.
    bool (*present)(radio_mux_fault_kind_t *kind, char *reason, size_t cap);
    // The boot steps (probe the installed image, the jumper, the automatic ROM recovery, reset into the RCP): ESP_OK
    // when the RCP runs; otherwise the fault's kind and reason (ESP_ERR_INVALID_STATE: the test build holds it in reset;
    // ESP_ERR_NOT_FOUND: the RCP cannot run).
    esp_err_t (*boot_steps)(radio_mux_fault_kind_t *kind, char *reason, size_t cap);
    // Push an image (`source`: a const rcp_fw_source_t *): rcp_sync's result, its text in `detail`. *base_only: the
    // RCP is left with its updater and no RCP firmware (c6_installer.bin through ESP-Hosted).
    esp_err_t (*push)(const void *source, char *detail, size_t cap, bool *base_only);
    // The ROM recovery of the base image: ESP_ERR_NOT_SUPPORTED without the maintenance jumper.
    esp_err_t (*rom_recover)(char *detail, size_t cap);
} radio_mux_builtin_t;

// Before radio_mux_start (ESP_ERR_INVALID_STATE after it). The descriptor is kept, not copied: it must outlive the mux.
esp_err_t radio_mux_set_builtin(const radio_mux_builtin_t *b);
// The built-in RCP's descriptor; NULL when none was set. Any task.
const radio_mux_builtin_t *radio_mux_builtin(void);

// Loads the channels from NVS, lists c6, creates the eventfds (the eventfd VFS must be registered), starts the owner
// task (MGMT_CORE), which installs the USB host from NET_CORE (its interrupt and the CDC driver's task run there, off
// the management core's interrupt-masking code) and binds registered instances to present channels. Once.
// The built-in RCP's channel starts in reinit for its boot steps: a short-lived task (mux_boot) runs them, then the
// channel is ready and its stored mode's instance bound once, or in fault (LOST to that instance).
esp_err_t radio_mux_start(void);

/* ---- consumer side (between attach and detach) ---- */

// Events go into `q` (items of radio_mux_ev_t, posted with timeout 0; frames that don't fit are dropped and counted)
// instead of the channel's own queue; events already waiting there move into `q`. Call it in attach. NULL goes back
// to the channel's queue.
esp_err_t radio_mux_bind_queue(radio_mux_chan_t *ch, QueueHandle_t q);
// The channel's eventfd: readable when its own queue has events (consumers without a bound queue) or a raw channel has
// bytes. The value means nothing; radio_mux_poll and radio_mux_read_raw clear it. -1 when the mux is not started.
int radio_mux_eventfd(radio_mux_chan_t *ch);
// Takes the next event of the channel's own queue without waiting; false when there is none (it then clears the
// eventfd, so take events until it returns false before waiting again). Until the transport's next attach or lease, the
// detached consumer can still take what it was sent (its GONE); from then on its handle gets false here and in
// radio_mux_read_raw, and ESP_ERR_INVALID_STATE from radio_mux_tx and radio_mux_bind_queue. The check and the take
// are one step against that next attach: an earlier handle never takes an event of the next consumer.
bool radio_mux_poll(radio_mux_chan_t *ch, radio_mux_ev_t *ev);
// Gives back the slot of an RX_FRAME event: the `slot` pointer the event carried. Allowed after GONE and detach, and
// through a handle of an earlier attach (the slot goes back to the channel's pool).
void radio_mux_release(radio_mux_chan_t *ch, uint8_t *slot);
// Sends one spinel frame (frame consumers; HDLC-encoded on USB) or bytes (raw consumers). The data is copied before
// the call returns. ESP_ERR_INVALID_STATE at once when the channel is not attached (gone, closed, reopening) or the
// handle is of an earlier attach, and only then: nothing was written (a transport's own ESP_ERR_INVALID_STATE comes
// back as ESP_FAIL, radio_mux_tx_transport_err); ESP_FAIL also for a write the device or transport failed;
// ESP_ERR_INVALID_SIZE for a frame above RADIO_MUX_FRAME_MAX. Blocks at most radio_mux_tx_timeout_ms (100 ms plus
// twice the write's wire time at the channel's baud) on a stalled device. On the C6: one SPI transaction, sent again a
// tick apart while the RCP is busy (at most 4 times, ESP_ERR_TIMEOUT), once more 15 ms after a bad header
// (ESP_ERR_INVALID_RESPONSE); about 60 ms at worst, plus a receive burst in progress.
esp_err_t radio_mux_tx(radio_mux_chan_t *ch, const uint8_t *data, size_t n);
// Asks for a reset (ignored through the handle of an earlier attach): a dongle is closed and reopened over USB (never
// DTR/RTS pulses, never VBUS), RESET when it is back,
// GONE when it did not reopen. The C6 gets a 20 ms pulse of its reset GPIO; the next SPI transaction carries the reset
// bit, and RESET is posted on the RCP's first header with its own reset bit (before its first frame), or after 2 s
// without one (the consumer's own timeouts then see a dead RCP). Does not wait for the reset; waits up to 1 s for room
// in the owner task's queue (ESP_ERR_TIMEOUT when there was none: nothing was asked). Not from the owner task.
esp_err_t radio_mux_reset(radio_mux_chan_t *ch);
// Raw channels: takes up to cap received bytes without waiting; 0 when there are none.
size_t radio_mux_read_raw(radio_mux_chan_t *ch, uint8_t *out, size_t cap);
const char *radio_mux_chan_id(const radio_mux_chan_t *ch);
// Whether the attached consumer gets frames on this channel (RX_FRAME, radio_mux_tx of frames) rather than raw bytes:
// always true on the C6, the consumer's `frames` on a USB channel. Valid from attach (the delivery kind is set before
// attach is called) until the next attach on the transport.
bool radio_mux_chan_frames(const radio_mux_chan_t *ch);
// The channel's stored rcp_proxy settings (for the rcp-proxy consumer, read at attach: a change re-attaches it).
void radio_mux_get_proxy(const radio_mux_chan_t *ch, radio_mux_proxy_settings_t *out);
// Passive firmware update (a consumer saw the spinel version): stored with source passive and the time. Does not wait.
// Ignored unless the handle is of the transport's current attach.
void radio_mux_note_firmware(radio_mux_chan_t *ch, const radio_mux_fw_record_t *fw);

/* ---- API side ---- */

typedef struct {
    bool has_mode;
    radio_mux_mode_t mode;
    bool has_baud;
    uint32_t baud;
    bool has_flow;
    bool flow;
    bool has_proxy;
    radio_mux_proxy_settings_t proxy;
    uint8_t proxy_fields; // RADIO_MUX_PROXY_F_* bits: which fields of `proxy` are given
    bool kick;            // never stored; radio_mux_set ignores it (device_api kicks through rcp_proxy)
} radio_mux_set_req_t;

typedef enum {
    RADIO_MUX_KIND_BUILTIN,
    RADIO_MUX_KIND_USB,
} radio_mux_kind_t;

typedef struct {
    radio_mux_fw_record_t rec;     // type, version, chip, baud that answered
    radio_mux_fw_source_t source;  // NONE: no record (the API's null)
    int64_t at;                    // seconds since the epoch; 0 when the board clock was not set
} radio_mux_firmware_t;

typedef struct {
    char id[RADIO_MUX_ID_MAX];
    radio_mux_kind_t kind;
    char label[RADIO_MUX_LABEL_MAX]; // USB product string, or a name made from the bridge
    bool present;                    // attached (USB) or answering (C6)
    bool supported;                  // a driver exists (USB) / the transport exists (C6)
    radio_mux_mode_t mode;           // the API's mode: UNAVAILABLE when not present or not supported
    radio_mux_mode_t stored_mode;
    radio_mux_state_t state;
    char reason[RADIO_MUX_REASON_MAX]; // the API's error; empty when none
    struct {
        uint32_t baud;     // the stored baud, or 460800 when none is stored
        bool flow_control; // the stored setting
    } line;
    struct {
        uint16_t vid, pid;
        char serial[RADIO_MUX_SERIAL_MAX];
        radio_mux_driver_t driver;
    } usb; // kind USB only
    struct {
        bool firmware_update, hw_reset, flow_control;
    } caps;
    radio_mux_proxy_settings_t proxy;
    const char *proxy_state;            // left NULL here; device_api fills it from rcp_proxy
    char proxy_peer[RADIO_MUX_PEER_MAX]; // left empty here; device_api fills it from rcp_proxy
    char proxy_last_error[RADIO_MUX_PROXY_ERROR_MAX]; // left empty here; device_api fills it from rcp_proxy
    radio_mux_firmware_t firmware;
    radio_mux_counters_t counters; // since boot
    // The orchestrator's view (design section 6): the channel's lifecycle and the bound instance's own state.
    radio_mux_chan_state_t chan_state;
    radio_mux_fault_kind_t fault_kind;      // fault: firmware or link
    radio_mux_work_t work;                  // reinit: the maintenance
    char chan_reason[RADIO_MUX_REASON_MAX]; // reinit: the work; fault: why; else ""
    char mode_state[16];                    // the instance's state ("" when nothing is bound or it reported none)
    char mode_reason[RADIO_MUX_REASON_MAX];
} radio_mux_chan_info_t;

// A registered mode instance as the read API lists it: a singleton once, a per-channel instance once per channel
// stored in its mode. Its state and reason are those of its channel (chans[chan].mode_state and .mode_reason);
// chan -1: no channel is stored in the mode (the instance is unbound).
typedef struct {
    radio_mux_mode_t mode;
    int8_t chan; // index into chans[], or -1
    bool reboot_on_change;
} radio_mux_mode_info_t;

typedef struct {
    uint32_t available_modes; // RADIO_MUX_MODE_BIT set: off, plus every mode with a registered consumer
    uint32_t reboot_modes;    // RADIO_MUX_MODE_BIT set: modes whose consumer reboots the board (reboot_on_change)
    size_t n;
    radio_mux_chan_info_t chans[RADIO_MUX_CHANNELS_MAX]; // c6 first, then USB channels in the order first seen
    size_t n_modes;
    radio_mux_mode_info_t modes[RADIO_MUX_RULES_MAX + RADIO_MUX_CHANNELS_MAX];
} radio_mux_snapshot_t;

// Every known channel (about 6 KB: allocate it, don't put it on a small stack). Zero channels before the start.
void radio_mux_get_snapshot(radio_mux_snapshot_t *out);

// Validates, stores in NVS and applies a change (attach, detach, re-attach; line settings reconfigure an open device).
// Settings for an unavailable channel are stored and apply on its return; a mode on it is refused. A refused change
// stores nothing. APPLY_REBOOT stores only: the caller reboots. MOVE (every change of where OpenThread runs, see
// radio_mux_orch_check_bind: otbr while another channel has it, otbr while none has it, the otbr channel leaving otbr)
// calls the mover set by radio_mux_set_otbr_mover from the calling task and stores no mode itself: a move or a start
// stores the target's otbr (and the source's off) once OpenThread stopped on the source, before it starts on the target
// (a move is a rebind: a failed start stays on the target, in fault), a stop stores the requested mode once OpenThread
// stopped (radio_mux_commit_otbr). MOVE means the target's probe passed (a stop: it was accepted) and the rest runs as
// a long operation (the target shows state starting, then running, or fault with the reason; a refused stop leaves the
// channel running otbr). With no mover:
// REFUSE_NOT_AVAILABLE. The mover's refusals: REFUSE_PROBE_FAILED (409 probe_failed), REFUSE_BUSY (a move or radio job
// runs, OpenThread starting or sniffing), REFUSE_FAILED (anything else); `reason` (may be NULL) then gets the mover's
// text, "" otherwise. A channel being moved to (leased) refuses every change with REFUSE_BUSY. On MOVE, moved_from gets
// the id OpenThread leaves ("" for a start, `id` itself for a stop; may be NULL). A start, move or stop goes alone:
// with line or rcp_proxy settings in the same request it is refused (REFUSE_BAD_VALUE, nothing stored, the mover not
// called). Waits for the owner task (up to 5 s to post, 5 s for the answer): REFUSE_FAILED when it could not post
// (nothing changes), REFUSE_PENDING when the answer did not come in time (the owner still applies the change when it
// gets to it; a start, move or stop does not run then).
radio_mux_verdict_t radio_mux_set(const char *id, const radio_mux_set_req_t *req, char *moved_from, size_t cap,
                                  char *reason, size_t reason_cap);

// Forgets an unplugged USB channel: erases its settings and removes it from the list. ESP_ERR_INVALID_STATE for the
// C6 or a present channel (and before radio_mux_start: radio_mux_started tells them apart), ESP_ERR_NOT_FOUND for an
// unknown id. Waits for the owner task: ESP_ERR_TIMEOUT when it could not post (nothing changes), ESP_ERR_NOT_FINISHED
// when the answer did not come in 5 s (the owner may still forget the channel).
esp_err_t radio_mux_forget(const char *id);

// Whether radio_mux_start has run (the API calls answer). Any task.
bool radio_mux_started(void);

// Copies the id of the channel whose stored mode is otbr (present or not) into `out` (cap: RADIO_MUX_ID_MAX fits any)
// and returns true; false with `out` "" when none (or before the start). The copy is taken under the registry's lock.
bool radio_mux_otbr_channel(char *out, size_t cap);

// br_core: the mover of OpenThread from channel `from` ("" for a start) to `to` ("" for a stop on `from`), called by
// radio_mux_set on the caller's task. It probes `to` (radio_mux_lease) before it returns: ESP_OK when the move was
// accepted (the rest runs on br_core's task), ESP_ERR_INVALID_RESPONSE when the probe failed, ESP_ERR_INVALID_STATE
// when busy; `reason` gets a short text then.
void radio_mux_set_otbr_mover(esp_err_t (*move)(const char *from, const char *to, char *reason, size_t cap));

/* ---- maintenance of the built-in RCP: the channel's reinit ---- */

// radio_mux_maint_t (what a maintenance did) and RADIO_MUX_SETTLE_MS: radio_mux_orch_core.h.

// Runs maintenance `work` on the built-in RCP's channel `id` as its reinit: RADIO_MUX_WORK_PUSH (`source` a
// const rcp_fw_source_t *) or RADIO_MUX_WORK_ROM_RECOVERY (`source` NULL). The instance bound to the channel is asked
// first (REINIT); only once it let go (at once, or after its drain) the channel enters reinit, the work runs on the
// calling task, then the built-in RCP's boot steps; the channel is ready (BACK to the instance, its state shown as
// "starting" until it reports) or in fault (LOST). Then this waits until the instance reports a state in which it runs
// or does not (radio_mux_orch_settle), at most RADIO_MUX_SETTLE_MS, and puts the outcome in `out` (mode, settle,
// mode_state, mode_reason). Refusals touch nothing and wait for nothing (out->ran false, out->settle NONE) and put the
// reason in `detail`:
// ESP_ERR_INVALID_STATE while another maintenance runs (the boot steps: "the radio is still booting") or OpenThread is
// being moved to the channel; the instance's own code and reason (RADIO_MUX_ERR_PEER_CONNECTED for the proxy's peer,
// ESP_ERR_INVALID_STATE for Thread); ESP_ERR_INVALID_STATE "<mode> did not let go of the radio in time" when it does
// not answer within RADIO_MUX_REINIT_REPLY_MS. ESP_ERR_NOT_FOUND for an id that is not the built-in RCP's,
// ESP_ERR_NOT_SUPPORTED without one, ESP_ERR_TIMEOUT when the owner task did not take the request. Otherwise the
// work's result. Blocks for the whole work and the wait (minutes for a push). Not from the owner task. `out` may be
// NULL.
esp_err_t radio_mux_maintain(const char *id, radio_mux_work_t work, const void *source, char *detail, size_t cap,
                             radio_mux_maint_t *out);
// Whether a maintenance runs (asked, or in reinit; the boot steps included). Any task.
bool radio_mux_maintenance_running(void);

/* ---- the OTBR lease (br_core, a move: R9) ---- */

// Takes the present channel `id` for OpenThread before its stored mode is otbr: the probe and the start of a move run
// on it while OpenThread may still run elsewhere. It must have no consumer attached (rcp-proxy on it: refused, "set it
// off first"), detected spinel firmware, no C6 job; the C6 is opened (out of reset, no reset pulse). Frames then go to
// the channel's own queue and eventfd (radio_mux_poll, radio_mux_eventfd, radio_mux_release), radio_mux_tx takes
// frames; radio_mux_reset works on it as on an attached channel (USB reopen or the C6's reset pulse, RESET into the
// lease's queue; a USB reopen that fails posts GONE and ends the lease). The channel shows otbr, state starting. An
// unplug posts GONE and ends the lease. *out gets the lease's handle (a new generation, as an attach);
// ESP_ERR_NOT_FOUND (unknown id), ESP_ERR_INVALID_STATE (not leasable), ESP_FAIL (the C6 did not open),
// ESP_ERR_TIMEOUT (the owner did not answer; nothing was leased), with `err` (may be NULL) saying why.
// Not from the owner task.
esp_err_t radio_mux_lease(const char *id, radio_mux_chan_t **out, char *err, size_t cap);
// Gives a leased channel back (no-op once committed or gone): delivery stops, the C6 returns to its stored mode (off:
// held in reset). `fault` (NULL or "" for none, e.g. after a refused probe) stays as the channel's reason, state fault,
// until its next mode change, plug-in or C6 job (drain_timeout, a start failure after the 202). Never dropped: it waits
// as long as it takes to post (and to allocate its record); the answer is waited for up to 5 s (later: logged).
void radio_mux_lease_end(radio_mux_chan_t *ch, const char *fault);
// The commit of a move, once OpenThread stopped on `from` and before it starts on the leased channel `to`: stores `to`
// in otbr and `from` (may be NULL or "", e.g. forgotten) in off, in one NVS commit, then unbinds Thread from `from`
// (GONE) and turns the lease into Thread's binding on `to` (BIND with the lease's handle; no GONE, no reopen, frames
// keep flowing).
// ESP_ERR_INVALID_STATE when `to` is not leased (unplugged meanwhile), the NVS error (nothing stored), ESP_ERR_TIMEOUT
// (nothing ran). With `to` NULL or "" it ends a stop, once OpenThread stopped on `from`: `from` is stored in the mode
// its radio_mux_set asked for (off or rcp-proxy), its otbr consumer detached (GONE) and that mode's attached;
// ESP_ERR_INVALID_STATE when `from` is not in otbr (any more).
esp_err_t radio_mux_commit_otbr(const char *from, const char *to);

#ifdef OTBR_TEST_BUILD
/* ---- test build only: CH340 experiments (2026-11-07) ---- */
// A control request on the open USB dongle (vendor or standard), under the port's tx_lock like a sender: a close or
// unplug meanwhile waits for it. ESP_ERR_INVALID_STATE when no dongle is open.
esp_err_t radio_mux_test_usb_ctrl(uint8_t bm_request_type, uint8_t b_request, uint16_t w_value, uint16_t w_index,
                                  uint16_t w_length, uint8_t *data);
// The owner's USB reset path (handle_reset's: close, open, RESET to the holder; a failed reopen detaches the consumer
// with GONE or ends the lease; counted by the dead-reset rule) on the dongle, when a consumer is attached or it is
// leased; ignored (logged) otherwise. Waits for nothing: ESP_OK means posted.
esp_err_t radio_mux_test_usb_reopen(void);
#endif

#ifdef __cplusplus
}
#endif
