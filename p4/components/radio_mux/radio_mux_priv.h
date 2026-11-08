// Shared between the owner task and registry (mux.c), the USB transport (usb_transport.c) and the C6 transport
// (c6_transport.c). Unless a function says otherwise, only the owner task calls it.
#pragma once

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"
#include "radio_mux.h"
#include "radio_mux_detect.h"
#include "radio_mux_hdlc.h"
#include "radio_mux_orch_core.h"
#include "radio_mux_pool.h"

// Room in front of a frame in its pool slot: the C6's 5-byte SPI header (R12). USB frames start at the same offset.
#define MUX_SLOT_HDR 5
#define MUX_CACHE_LINE 64 // CONFIG_CACHE_L1_CACHE_LINE_SIZE and CONFIG_CACHE_L2_CACHE_LINE_SIZE on the P4
#define MUX_ALIGN_UP(n, a) ((((n) + (a) - 1) / (a)) * (a))
// A full SPI transaction (header + largest frame) plus the HDLC decoder's two CRC bytes, rounded to the cache line.
#define MUX_SLOT_SIZE MUX_ALIGN_UP(MUX_SLOT_HDR + RADIO_MUX_FRAME_MAX + RADIO_MUX_HDLC_CRC_LEN, MUX_CACHE_LINE)

typedef enum {
    MUX_RX_DISCARD, // nobody reads: received bytes are dropped
    MUX_RX_DETECT,  // bytes into the ring for the detection (owner task), no eventfd
    MUX_RX_RAW,     // bytes into the ring for a raw consumer, eventfd poked
    MUX_RX_FRAMES,  // HDLC decoded into pool slots, RX_FRAME posted
} mux_rx_mode_t;

typedef struct {
    uint16_t vid, pid;
    uint8_t addr;
    radio_mux_driver_t driver;
    char serial[RADIO_MUX_SERIAL_MAX];
    char product[RADIO_MUX_LABEL_MAX];
} mux_usb_dev_t;

// The answer to a reinit as a message carries this much of the instance's reason.
#define MUX_REPLY_REASON_MAX 72

// A registered mode instance (orch.c).
typedef struct orch_inst {
    radio_mux_instance_t inst;
    uint32_t seq[RADIO_MUX_CHANNELS_MAX]; // inputs sent: per channel slot (per_channel), else [0]
    bool held[RADIO_MUX_CHANNELS_MAX];    // BIND sent on that slot and no UNBIND since (a LOST keeps it)
} orch_inst_t;

// One transport: the C6 or the USB port (one device open at a time). Runtime state only; the stored settings live
// in the channel records.
typedef struct mux_port {
    radio_mux_kind_t kind;
    radio_mux_attach_t att;                // owner
    struct radio_mux_chan *chan;           // owner: the channel on this transport, NULL while ABSENT
    orch_inst_t *inst;                     // owner: the instance bound to the channel on this port (NULL: none)
    mux_usb_dev_t dev;                     // owner: the device on the port (USB)
    mux_usb_dev_t pending;                 // owner: the device to probe next (PLUG, or deferred while CLOSING)
    char fault[RADIO_MUX_REASON_MAX];      // owner: why the device is not open or not attached; "" when fine
    radio_mux_fault_kind_t fault_kind;     // owner: what `fault` is: link or firmware (a channel fault), NONE (a mode's)
    char warn[RADIO_MUX_REASON_MAX];       // owner: a warning of the last open (flow control unsupported, baud off)
    radio_mux_detect_t det;                // owner: detection while PROBING
    int64_t det_wait_ms;
    uint32_t det_baud;
    int64_t presence_check_us;
    bool busy; // owner, C6: in reinit (maintenance: the boot steps, a push, a ROM recovery)
    radio_mux_work_t work; // owner, C6: the maintenance while busy
    // owner: the channel on this port is leased for OTBR (radio_mux_lease): frames to its own queue and eventfd, no
    // consumer, until radio_mux_lease_end or radio_mux_commit_otbr; an unplug posts GONE and ends it
    bool leased;

    // The generation of the attach (or lease) the queue, eventfd, ring and TX belong to: set at attach, kept through
    // detach and close (so the old consumer can still take its GONE), replaced at the next attach (mux_new_attach,
    // which holds tx_lock and ev_lock for it). Calls through a handle of any other generation get false, 0 or
    // ESP_ERR_INVALID_STATE. 0: nothing attached yet.
    _Atomic uint32_t att_gen;
    radio_mux_chan_t *att_handle; // owner: the handle of att_gen (what detach and a lease's commit pass on)
    // Consumers' takes (radio_mux_poll, radio_mux_read_raw, radio_mux_bind_queue) check att_gen and take under it; the
    // owner holds it while it drains the queue and moves att_gen on, so an earlier handle never takes a later event.
    SemaphoreHandle_t ev_lock;
    atomic_bool dev_gone; // the transport saw the device disconnect (set by its callback, cleared by the open)
    // USB: a CDC_ACM_HOST_ERROR stopped the device's polling (set by its callback, cleared by the owner's reopen, the
    // close and the open); usb_error_us: when the owner last reopened for one (0: never), owner only.
    atomic_bool usb_error;
    int64_t usb_error_us;

    // Delivery. Changed by the owner with rx_lock held, read by the transport with rx_lock held.
    QueueHandle_t evq;   // the channel's own queue (consumers without a bound queue)
    QueueHandle_t bound; // the consumer's queue, NULL = evq + eventfd
    int efd;

    // Receive path: the transport's callback holds rx_lock for its whole run, the owner while it changes the mode.
    SemaphoreHandle_t rx_lock;
    mux_rx_mode_t rx_mode;
    radio_mux_hdlc_dec_t dec;
    uint8_t *dec_slot;   // slot the decoder writes into (at MUX_SLOT_HDR); NULL = into `discard`
    uint8_t *discard;    // decoder target while the pool is empty: the frame is parsed and dropped
    uint32_t dec_errors; // crc_errors + overruns already added to the counters
    // USB: reset requests with nothing received since (RADIO_MUX_USB_DEAD_RESETS); cleared by every RX frame or raw
    // byte and at every attach, counted by the owner's handle_reset.
    radio_mux_reset_count_t reset_count;
    RingbufHandle_t ring; // raw and detection bytes

    // Pool and counters: under `lock` (taken by the transport, the consumers' release and tx, and the snapshot).
    portMUX_TYPE lock;
    radio_mux_pool_t pool;
    radio_mux_counters_t ctr;
    int64_t uart_err_log_us; // USB, the CDC driver task only: when the last UART error warning was logged (0: never)

    // Transmit: tx_lock serialises senders and the owner's close (on the C6 also the INT-driven receive transactions:
    // one SPI transaction at a time); tx_ok is false whenever a sender must fail at once.
    SemaphoreHandle_t tx_lock;
    atomic_bool tx_ok;
    bool tx_frames; // set at attach: the consumer sends frames (HDLC-encoded on USB)
    uint8_t *tx_buf;
    size_t tx_cap;
} mux_port_t;

typedef enum {
    MUX_MSG_NEW_DEV,  // new_dev_cb: a USB device appeared
    MUX_MSG_GONE,     // event_cb: the open device was disconnected
    MUX_MSG_USB_ERROR, // event_cb: a transfer error stopped the open device's polling (p->usb_error is set)
    MUX_MSG_SET,      // radio_mux_set
    MUX_MSG_FORGET,   // radio_mux_forget
    MUX_MSG_RESET,    // radio_mux_reset
    MUX_MSG_NOTE_FW,  // radio_mux_note_firmware
    MUX_MSG_MAINT_ASK,    // radio_mux_maintain: may the channel enter reinit?
    MUX_MSG_MAINT_DONE,   // radio_mux_maintain or the boot task: the work and the boot steps ended
    MUX_MSG_LEASE,        // radio_mux_lease
    MUX_MSG_LEASE_END,    // radio_mux_lease_end
    MUX_MSG_COMMIT_OTBR,  // radio_mux_commit_otbr
    MUX_MSG_REINIT_REPLY, // radio_mux_reinit_reply
#ifdef OTBR_TEST_BUILD
    MUX_MSG_TEST_REOPEN,  // radio_mux_test_usb_reopen
#endif
} mux_msg_type_t;

typedef struct mux_call mux_call_t;

typedef struct {
    mux_msg_type_t type;
    union {
        mux_usb_dev_t dev;        // NEW_DEV
        const void *hdl;          // GONE: the CDC handle that went away
        mux_call_t *call;         // SET, FORGET, MAINT_ASK, MAINT_DONE, LEASE, LEASE_END, COMMIT_OTBR
        radio_mux_chan_t *h;      // RESET: the consumer's handle
        struct {
            radio_mux_chan_t *h;
            radio_mux_fw_record_t fw;
        } note;                   // NOTE_FW
        struct {
            radio_mux_chan_t *h;
            uint32_t seq;
            esp_err_t verdict;
            char reason[MUX_REPLY_REASON_MAX];
        } reply;                  // REINIT_REPLY
    };
} mux_msg_t;

// A channel record. Written by the owner task (under g_mux.lock where other tasks read it), read under the lock.
struct radio_mux_chan {
    bool used;
    bool remembered; // stored in NVS (unsupported devices never are)
    char id[RADIO_MUX_ID_MAX];
    radio_mux_kind_t kind;
    mux_port_t *port; // the transport of its kind (for the whole firmware life)
    // Stored settings.
    radio_mux_mode_t mode;
    uint32_t baud; // 0 = none stored (460800 applies)
    bool flow;
    radio_mux_proxy_settings_t proxy;
    radio_mux_firmware_t fw;
    int64_t first_seen, last_seen;
    mux_usb_dev_t usb;
    char label[RADIO_MUX_LABEL_MAX];
    // Runtime, published through the snapshot.
    bool present, supported;
    radio_mux_mode_t eff_mode;
    radio_mux_state_t state;
    char reason[RADIO_MUX_REASON_MAX];
    radio_mux_counters_t ctr; // while not on its port
    radio_mux_mode_t leave_mode; // owner: the mode a stop of OpenThread on it stores at its commit (radio_mux_set)
    bool det_failed; // runtime: its last detection found nothing and the earlier record was kept (shown as the reason)
    // Runtime, the orchestrator's view (radio_mux_chan_info_t): set by refresh, mode_* also by radio_mux_report.
    radio_mux_chan_state_t chan_state;
    radio_mux_fault_kind_t fault_kind;
    radio_mux_work_t work;
    char chan_reason[RADIO_MUX_REASON_MAX];
    char mode_state[16];
    char mode_reason[RADIO_MUX_REASON_MAX];
};

struct mux_call {
    SemaphoreHandle_t done;
    atomic_int refs; // the caller and the owner; the last one frees
    char id[RADIO_MUX_ID_MAX];
    radio_mux_set_req_t req;
    bool have_mover;
    bool stop; // SET: the MOVE is a stop of OpenThread on this channel (it leaves otbr), not a move to it
    radio_mux_work_t work;       // MAINT_ASK: the maintenance
    esp_err_t steps;             // MAINT_DONE: what the built-in RCP's boot steps returned
    radio_mux_fault_kind_t kind; // MAINT_DONE: their fault's kind (reason: its text)
    atomic_int claim; // MAINT_ASK, LEASE, COMMIT_OTBR: MUX_CALL_PENDING until the owner takes it (TAKEN) or the caller
    // gives up
    radio_mux_verdict_t verdict;
    esp_err_t err;
    char from[RADIO_MUX_ID_MAX];         // SET: the source of a MOVE; COMMIT_OTBR: the channel that leaves otbr
    radio_mux_chan_t *handle;            // LEASE: the lease's handle; LEASE_END: the lease to give back
    char reason[RADIO_MUX_REASON_MAX];   // LEASE: why it was refused; LEASE_END: the fault to show ("" none);
    // MAINT_ASK: why it was refused; MAINT_DONE: the boot steps' fault
};

// mux_call_t.claim (calloc: PENDING).
#define MUX_CALL_PENDING 0
#define MUX_CALL_TAKEN 1
#define MUX_CALL_ABANDONED 2

#define MUX_RX_RING_SIZE (16 * 1024) // raw and detection bytes: a 100 ms burst at 921600 baud stays well inside

// The mux's shared state (mux.c owns it).
typedef struct {
    QueueHandle_t queue;   // the owner task's messages
    SemaphoreHandle_t lock; // the channel records: written by the owner, read by the API and consumers
    struct radio_mux_chan chans[RADIO_MUX_CHANNELS_MAX]; // c6 in slot 0
    mux_port_t c6;
    mux_port_t usb;
    orch_inst_t insts[RADIO_MUX_RULES_MAX]; // the registered instances (orch.c)
    size_t n_insts;
    radio_mux_reinit_run_t reinit;           // owner: the maintenance's reinit run
    mux_call_t *maint_call;                  // owner: the MAINT_ASK waiting for the instance's answer
    radio_mux_work_t maint_work;             // owner: its work
    orch_inst_t *maint_inst;                 // owner: the instance asked (its mode names a timeout)
    atomic_bool maint_running;               // a maintenance is asked or runs (radio_mux_maintenance_running)
    bool maint_ending; // owner: handle_maint_done binds the C6's instance again (orch_bind shows it "starting")
    const radio_mux_builtin_t *builtin;      // the board's built-in RCP (radio_mux_set_builtin), or NULL
    radio_mux_rules_t rules;
    atomic_bool started;
    esp_err_t (*_Atomic mover)(const char *from, const char *to, char *reason, size_t cap);
    uint32_t gen; // owner: the generation of the last attach or lease (radio_mux_gen_next), on any transport
} mux_t;

extern mux_t g_mux;

/* ---- mux.c ---- */

// Any task or callback, never blocks: false when the queue is full or the mux is not started.
bool mux_post(const mux_msg_t *m);
void mux_lock(void);
void mux_unlock(void);
// Seconds since the epoch, 0 when the board clock was never set.
int64_t mux_now_epoch(void);
// The stored baud, or 460800 when none is stored.
uint32_t mux_eff_baud(const struct radio_mux_chan *ch);
// Any task: the channel record and transport behind a handle of any generation (*gen set); NULL when `h` is no handle.
struct radio_mux_chan *mux_handle_chan(const radio_mux_chan_t *h, mux_port_t **port, uint32_t *gen);
// Owner, before a consumer's attach or a lease on p->chan: what the earlier consumer left in the channel's own queue
// goes, the queue is unbound, and a new generation starts (tx_lock and ev_lock held for it, so an earlier handle's
// transmit or take in progress ends first). Returns the new handle (also in p->att_handle).
radio_mux_chan_t *mux_new_attach(mux_port_t *p);

/* ---- orch.c (the orchestrator runtime; owner task unless said otherwise) ---- */

// The instance registered for `mode`; NULL when none is.
orch_inst_t *orch_find(radio_mux_mode_t mode);
// BIND (or BACK, when the instance holds the binding of p->chan) to `oi` with the new handle `h`.
void orch_bind(mux_port_t *p, orch_inst_t *oi, radio_mux_chan_t *h);
// UNBIND (the binding goes) or LOST (it is kept) to `oi`, bound to p->chan through p->att_handle. GONE was posted
// before.
void orch_leave(mux_port_t *p, orch_inst_t *oi, radio_mux_input_type_t type, radio_mux_fault_kind_t kind,
                const char *reason);
// UNBIND to the instance of `mode` that holds the binding of channel `ch` while nothing is attached there (a move
// leaving an absent dongle). No-op when it holds none.
void orch_release(struct radio_mux_chan *ch, radio_mux_mode_t mode);
// Channel slot `slot` was forgotten or reused for another device: no instance holds a binding there any more.
void orch_forget_slot(size_t slot);
// A REINIT_REPLY message.
void orch_reinit_reply(radio_mux_chan_t *h, uint32_t seq, esp_err_t verdict, const char *reason);
// Maintenance asks for the channel on p: REINIT to `oi` (the instance bound to it, NULL for none) with the next seq,
// and the run's verdict (RUN at once without an instance, else WAIT).
radio_mux_reinit_act_t orch_reinit_ask(mux_port_t *p, orch_inst_t *oi, radio_mux_work_t work, int64_t now_ms);
// Maintenance runs on p's channel with no instance bound to it: REINIT as a notice to the instance of its stored mode
// (its answer is not waited for). Thread shows the maintenance (STARTING) where it was in fault.
void orch_reinit_notice(mux_port_t *p, radio_mux_work_t work);
// The channel `ch` ended its reinit in fault: LOST (`kind`, `reason`) to the instance of its stored mode, bound or not
// (it hears why its channel cannot be used). `h`: the channel's last handle.
void orch_lost(struct radio_mux_chan *ch, radio_mux_chan_t *h, radio_mux_fault_kind_t kind, const char *reason);
// Under mux_lock: the instances list of the snapshot.
void orch_fill_modes(radio_mux_snapshot_t *out);

/* ---- mux.c: the maintenance runner (owner) ---- */

// The pending maintenance's verdict: RUN (the channel enters reinit) or REFUSE (`verdict`, `reason`); WAIT and IGNORE
// change nothing.
void mux_maint_answer(radio_mux_reinit_act_t act, esp_err_t verdict, const char *reason);

/* ---- mux_api.c ---- */

// Any task but the owner: the built-in RCP's boot steps on the channel in reinit, then MAINT_DONE (never dropped).
void mux_maint_finish(const char *id);

/* ---- mux_deliver.c ---- */

// Transport callback context, rx_lock held: bytes received from a byte-stream transport (USB), handled per rx_mode.
void mux_rx_bytes(mux_port_t *p, const uint8_t *data, size_t len);
// Any task: takes up to cap bytes from the port's ring without waiting.
size_t mux_ring_take(mux_port_t *p, uint8_t *out, size_t cap);
void mux_ring_flush(mux_port_t *p);
// Any task: clears the eventfd, and sets it again when an event or ring bytes still wait.
void mux_rearm(mux_port_t *p);
// Any task: gives back the slot a frame pointer lies in (R12: frames sit at an offset inside their slot).
void mux_slot_put(mux_port_t *p, uint8_t *frame);
// Any task: a free slot of the port's pool, NULL when none is (or the pool is not allocated yet).
uint8_t *mux_slot_take(mux_port_t *p);
// Frame transport (C6) task, rx_lock held: one received frame of len bytes at slot + MUX_SLOT_HDR, posted as RX_FRAME
// while a frame consumer is attached (else the slot goes back). slot NULL: the frame went into the discard buffer
// (the pool was empty) and counts as a drop.
void mux_rx_frame(mux_port_t *p, uint8_t *slot, size_t len);
// Frame transport (C6) task, rx_lock held: RESET to the attached consumer; nothing while none is attached.
void mux_rx_reset(mux_port_t *p);
// Owner: what the receive path does with incoming bytes; waits for a callback in progress.
void mux_set_rx(mux_port_t *p, mux_rx_mode_t mode);
// Owner: GONE or RESET to the attached consumer.
void mux_post_ctl(mux_port_t *p, radio_mux_ev_type_t type);
// Counts n receive errors (bad headers, CRC) on the port.
void mux_count_rx_errors(mux_port_t *p, uint32_t n);
// Owner: empties the channel's own queue, slots back to the pool.
void mux_drain_evq(mux_port_t *p);

/* ---- mux_nvs.c (owner) ---- */

esp_err_t mux_store_chan(struct radio_mux_chan *ch);
// Both channels' blobs (b may be NULL or a) in one NVS handle and one commit (a move's two mode changes).
esp_err_t mux_store_chans(struct radio_mux_chan *a, struct radio_mux_chan *b);
esp_err_t mux_erase_chan(struct radio_mux_chan *ch);
void mux_chan_defaults(struct radio_mux_chan *ch, const char *id, radio_mux_kind_t kind);
// Lists c6 and every remembered channel from NVS. OTBR stored on more than one channel keeps the first in slot order
// (the C6 first); the others are stored off.
void mux_load_chans(void);

/* ---- mux_api.c ---- */

// Drops one reference of a call record; the last one frees it.
void mux_call_put(mux_call_t *call);

/* ---- usb_transport.c ---- */

// From the owner task: the usb_lib task on NET_CORE, which runs usb_host_install there (so the USB interrupt lands on
// NET_CORE), then the CDC driver (its task on NET_CORE too) with its new-device callback.
esp_err_t usb_transport_start(void);
// Opens `dev` (callbacks bound to `p`), then line coding at baud 8N1 with read-back, flow control (CP210x only) and
// DTR+RTS asserted together once. The first open allocates the port's buffers (pool, discard, TX, ring) for good. On an error the device is closed and `err` says why; `err` may also hold a warning
// (flow control unsupported, baud read back close) with ESP_OK.
esp_err_t usb_transport_open(mux_port_t *p, const mux_usb_dev_t *dev, uint32_t baud, bool flow, char *err, size_t cap);
// New line settings on the open device. A failure (or a baud read back far off) closes the device.
esp_err_t usb_transport_line(mux_port_t *p, uint32_t baud, bool flow, char *err, size_t cap);
// The single closer: takes tx_lock so no sender is inside the driver, then cdc_acm_host_close once. No-op when closed.
// A close the driver refuses keeps the handle for usb_transport_close_retry; until it closes, nothing opens (an open
// fails with RADIO_MUX_USB_CLOSE_PENDING_REASON, or RADIO_MUX_USB_CLOSE_FAILED_REASON once the retries are spent; an
// open then tries the close once more first).
void usb_transport_close(mux_port_t *p);
// Whether a failed close is being retried.
bool usb_transport_close_pending(void);
// Owner loop: closes a device whose close failed again when a retry is due (radio_mux_close_retry_t). DONE once it
// closed, GAVE_UP once after the last failed retry; IDLE, WAIT otherwise.
radio_mux_close_act_t usb_transport_close_retry(void);
bool usb_transport_is_open(void);
bool usb_transport_is(const void *hdl);
// Any task, with p->tx_lock held. ESP_ERR_INVALID_STATE when no device is open.
esp_err_t usb_transport_write(const uint8_t *data, size_t n);
// Whether a device with this address is still enumerated (a device that is not open sends no disconnect event).
bool usb_transport_dev_attached(uint8_t addr);

/* ---- c6_transport.c ---- */

// From radio_mux_start, before the owner task: the C6 task (NET_CORE), which installs the GPIO ISR service on its core.
esp_err_t c6_transport_start(void);
// Owner. Opens the C6 for g_mux.c6 (R8: only when a consumer attaches): allocates the port's frame pool on the first
// open, drives the C6's UPDATE pin high and its reset GPIO high (the C6 runs), SPI2 (mode 0, P4_RCP_SPI_HZ, its ISR on
// NET_CORE), the INT pin's falling-edge interrupt. The first transaction carries the reset bit. A failure leaves it
// closed (the reset GPIO as it was) and `err` says why.
esp_err_t c6_transport_open(char *err, size_t cap);
// Owner. Closes SPI2 and the INT interrupt (waits for a transaction in progress) and holds the C6 in reset (mode off).
void c6_transport_close(void);
// Owner. Closes like c6_transport_close, but the C6 keeps running: its reset GPIO is driven high when the mux set it
// up, else left alone (rcp_sync or OpenThread's own SPI path may use the C6 then). No-op on SPI when closed.
void c6_transport_release(void);
bool c6_transport_is_open(void);
// With g_mux.c6.tx_lock held. Sends one spinel frame in one full-duplex transaction; a frame the RCP sends in it is
// delivered like any other. Retries while the RCP does not take it (busy: accept below the length) a few times.
// ESP_ERR_INVALID_STATE when closed, ESP_ERR_TIMEOUT when the RCP never took it, ESP_ERR_INVALID_RESPONSE when its
// header stayed invalid.
esp_err_t c6_transport_tx(const uint8_t *frame, size_t n);
// Owner. Pulses the reset GPIO; the next transaction carries the reset bit, and the C6 task posts RESET on the RCP's
// first valid header after it (or, when none comes within C6_RESET_WAIT_MS, posts it anyway). ESP_ERR_INVALID_STATE
// when closed.
esp_err_t c6_transport_reset(void);
