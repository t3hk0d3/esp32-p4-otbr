#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * A small spinel client that drives an OpenThread RCP for ziggurat, over whole spinel frames (header byte first, no
 * HDLC, no SPI header). Every encoding follows OpenThread's RadioSpinel (ESP-IDF 6.1, lib/spinel/radio_spinel.cpp).
 *
 * Threading: not thread-safe. Every call, and every on_frame / on_tick, comes from one task (the zigbee task).
 *
 * Property requests (attach, configure, set_channel, set_promiscuous, src_match_*) are synchronous, as in RadioSpinel:
 * the client sends one request, then calls ops->wait until its reply has been passed to zigbee_spinel_on_frame or the
 * request timed out, and returns the RCP's status: 0 on success as RadioSpinel's HandleWaitingResponse defines it
 * (LAST_STATUS OK, or the property's value with the expected command), the RCP's spinel status (> 0) when it refused,
 * SPINEL_STATUS_FAILURE (1) for any other reply, or a negative ZIGBEE_SPINEL_ERR_*: a local refusal, a timeout (which
 * also requests a channel reset), or an abort through zigbee_spinel_abort_wait (no reset requested). Only a refusal
 * (> 0) is the RCP's answer; after a negative code a call stops and changes no stored state it has not already
 * committed (except a source-match add, which stores its entry: zigbee_spinel_src_match_add_short). One property
 * request runs at a time.
 *
 * Transmit returns at once; its answer comes through the sink. An energy scan's three property requests are
 * synchronous like the others; only its result comes through the sink. One transmit may be in flight while a property
 * request runs.
 *
 * The sink may be called from inside ops->wait: a received frame (rx), a transmit answer (tx_done) or an energy scan
 * result (scan_done) that arrives while a property request waits is delivered then. That is safe for ziggurat: its
 * ziggurat_radio_rx / _tx_done / _energy_scan_done only push into critical-section channels (deliver_* in
 * ziggurat-phy-otlink). The sink must not call back into this client.
 *
 * Radio resets and loss (the mux's RESET, GONE and attach events; the board passes them in, also from inside
 * ops->wait):
 *  - RESET (zigbee_spinel_on_reset, or an unsolicited LAST_STATUS in the reset range from the RCP): a transmit in
 *    flight completes as failed and an energy scan in progress completes with ZIGBEE_SPINEL_SCAN_FAILED_RSSI, then the
 *    attach sequence runs again and replays the stored configuration and source-match table.
 *  - GONE (zigbee_spinel_on_gone): the open operations fail the same way and the client has no radio: transmit and
 *    energy_scan return nonzero at once, configuration and source-match edits are stored (for the replay) and return
 *    0 without sending. The next zigbee_spinel_attach is the radio coming back: attach sequence and replay.
 *  - An event that arrives inside ops->wait ends that wait (the call returns ZIGBEE_SPINEL_ERR_ABORTED, no channel
 *    reset requested), and every later request of that call fails the same way without sending, so the call unwinds.
 *    The attach sequence then runs once that outermost client call has unwound, before it returns (so sink.ready may
 *    be reported from the end of any client call). A RESET inside the attach sequence restarts it from CMD_RESET.
 */

/** Timeout of one property request (RadioSpinel kMaxWaitTime). */
#define ZIGBEE_SPINEL_REQUEST_TIMEOUT_US (2000 * 1000LL)
/** Timeout of a transmit's answer (RadioSpinel kTxWaitUs, OPENTHREAD_SPINEL_CONFIG_RCP_TX_WAIT_TIME_SECS = 5). */
#define ZIGBEE_SPINEL_TX_TIMEOUT_US (5000 * 1000LL)
/** An energy scan's result is due this long after the scan's duration has passed. */
#define ZIGBEE_SPINEL_SCAN_GRACE_US (2000 * 1000LL)
/** The protocol major version the client speaks (SPINEL_PROTOCOL_VERSION_THREAD_MAJOR). */
#define ZIGBEE_SPINEL_PROTOCOL_MAJOR 4
/** Largest PSDU, FCS included (OT_RADIO_FRAME_MAX_SIZE). */
#define ZIGBEE_SPINEL_PSDU_MAX 127
/** Entries of each source-match table the client keeps for replay. The RCP's own refusal is its real limit. */
#define ZIGBEE_SPINEL_SRC_MATCH_MAX 128
/** Negative results of a property request (and of the calls that make one). */
#define ZIGBEE_SPINEL_ERR_LOCAL (-1)   // refused locally: busy, no TID, the frame did not fit, or send failed
#define ZIGBEE_SPINEL_ERR_TIMEOUT (-2) // no reply in ZIGBEE_SPINEL_REQUEST_TIMEOUT_US; a channel reset was requested
#define ZIGBEE_SPINEL_ERR_ABORTED (-3) // zigbee_spinel_abort_wait ended the wait
/** The peak RSSI reported through sink.scan_done for a scan the radio's reset or loss ended (OT_RADIO_RSSI_INVALID). */
#define ZIGBEE_SPINEL_SCAN_FAILED_RSSI 127
/** Largest spinel frame the client builds or keeps a reply value of. */
#define ZIGBEE_SPINEL_FRAME_MAX 256

typedef enum {
    ZIGBEE_TX_ACKED,
    ZIGBEE_TX_NO_ACK,
    ZIGBEE_TX_CCA,
    ZIGBEE_TX_ABORTED,
    ZIGBEE_TX_FAILED,
} zigbee_tx_result_t;

typedef struct {
    /** Hands one bare spinel frame to the channel (the callee copies it). 0 on success. */
    int (*send)(void *ctx, const uint8_t *frame, size_t len);
    /** Asks for a channel reset (answered later by the mux's RESET: zigbee_spinel_on_reset, or GONE). */
    void (*request_reset)(void *ctx);
    /** Monotonic microseconds. */
    int64_t (*now_us)(void *ctx);
    /**
     * Blocks until the next radio event has been passed to the client (zigbee_spinel_on_frame, _on_reset, _on_gone,
     * _attach) or until now_us() reaches `until_us`. 0 after an event was delivered, nonzero when the deadline passed
     * with none. The client calls it in a loop while a property request waits.
     */
    int (*wait)(void *ctx, int64_t until_us);
    void *ctx;
} zigbee_spinel_ops_t;

typedef struct {
    /** Attach finished. `compatible` false with `reason` (owned by the client) when the RCP cannot serve; NULL else. */
    void (*ready)(void *ctx, bool compatible, const char *reason);
    /** A received frame: PSDU without FCS, with the mux's receive time. */
    void (*rx)(void *ctx, const uint8_t *psdu, size_t len, uint8_t channel, int8_t rssi, uint8_t lqi,
               uint64_t time_us);
    void (*tx_done)(void *ctx, zigbee_tx_result_t r);
    void (*scan_done)(void *ctx, int8_t max_rssi);
    void *ctx;
} zigbee_spinel_sink_t;

typedef struct {
    uint8_t channel;
    int8_t tx_power_dbm;
    uint16_t pan_id, short_addr;
    uint8_t ext_addr[8]; // ziggurat's (big-endian, otExtAddress) order; sent reversed, as RadioSpinel receives it
    bool promiscuous;
} zigbee_radio_config_t;

typedef enum {
    ZIGBEE_SPINEL_WAIT_IDLE,    // no property request
    ZIGBEE_SPINEL_WAIT_PENDING, // sent, no reply yet
    ZIGBEE_SPINEL_WAIT_OK,
    ZIGBEE_SPINEL_WAIT_FAILED,  // the RCP's error status, or an unexpected reply
    ZIGBEE_SPINEL_WAIT_ABORTED, // zigbee_spinel_abort_wait
} zigbee_spinel_wait_t;

/** The client's state. Caller-allocated; treat as opaque. */
typedef struct {
    zigbee_spinel_ops_t ops;
    zigbee_spinel_sink_t sink;

    bool radio;          // a channel is attached: from zigbee_spinel_attach until zigbee_spinel_on_gone
    bool attached;       // the attach sequence enabled the PHY and the raw stream
    bool attach_pending; // a RESET or an attach arrived: the attach sequence runs when the outermost call unwinds
    uint8_t depth;       // nesting of client calls that may wait
    uint32_t resets;     // RESETs handled (the mux's and the RCP's own)
    bool have_eui64;
    uint8_t eui64[8];
    uint32_t radio_caps;
    char reason[96];

    uint16_t tids_in_use; // bit n: TID n is outstanding
    uint8_t next_tid;     // 1..15

    // The one synchronous property request.
    zigbee_spinel_wait_t wait_state;
    bool wait_for_reset;    // attach: waiting for the RCP's reset status (TID 0)
    uint8_t wait_tid;
    uint32_t wait_key;
    uint32_t wait_cmd;      // the command a success reply carries (VALUE_IS, VALUE_INSERTED)
    uint32_t reply_status;  // the spinel status of the reply (SPINEL_STATUS_OK when the value came back)
    uint8_t reply[ZIGBEE_SPINEL_FRAME_MAX];
    size_t reply_len;

    // The transmit in flight.
    bool tx_in_flight;
    uint8_t tx_tid;
    int64_t tx_deadline_us;

    // Receive re-arm after a transmit answer, on RCPs without rx-on-when-idle (sent without waiting).
    uint8_t rearm_tid;
    int64_t rearm_deadline_us;

    // The energy scan in progress.
    bool scanning;
    bool scan_starting;  // energy_scan's own requests still run: a failure is its return value, not scan_done
    int64_t scan_deadline_us;

    // Stored for replay.
    bool have_config;
    zigbee_radio_config_t config;
    size_t src_short_count, src_ext_count;
    uint16_t src_short[ZIGBEE_SPINEL_SRC_MATCH_MAX];
    uint8_t src_ext[ZIGBEE_SPINEL_SRC_MATCH_MAX][8]; // ziggurat's order

    uint8_t frame[ZIGBEE_SPINEL_FRAME_MAX]; // request being built
    uint8_t psdu[ZIGBEE_SPINEL_PSDU_MAX];   // transmit PSDU with its FCS
} zigbee_spinel_t;

/** Resets the client; nothing is sent. `ops` and `sink` are copied. */
void zigbee_spinel_init(zigbee_spinel_t *s, const zigbee_spinel_ops_t *ops, const zigbee_spinel_sink_t *sink);

/**
 * The mux attached the channel (the first time, or the radio coming back after GONE). Runs the attach sequence:
 * CMD_RESET, then PROTOCOL_VERSION, NCP_VERSION, CAPS, RADIO_CAPS, HWADDR; PHY_ENABLED and MAC_RAW_STREAM_ENABLED;
 * then replays the stored configuration and source-match table. Ends with sink.ready, unless a request timed out
 * (then the channel reset is requested) or was aborted: then the sequence stops there, ready is not reported and the
 * stored configuration and table are kept as they were. A transmit or energy scan still open from before completes as
 * failed first. Called from inside ops->wait, it ends that wait and runs once the outermost call has unwound.
 */
void zigbee_spinel_attach(zigbee_spinel_t *s);

/**
 * Sends PHY_CHAN, PHY_TX_POWER, MAC_15_4_PANID, MAC_15_4_SADDR, MAC_15_4_LADDR, MAC_PROMISCUOUS_MODE,
 * MAC_SRC_MATCH_ENABLED and (with the rx-on-when-idle radio cap) MAC_RX_ON_WHEN_IDLE_MODE, stopping at the first
 * refusal, and stores the configuration for replay. Not attached: stores it and returns 0.
 */
int zigbee_spinel_configure(zigbee_spinel_t *s, const zigbee_radio_config_t *config);
int zigbee_spinel_set_channel(zigbee_spinel_t *s, uint8_t channel);
int zigbee_spinel_set_promiscuous(zigbee_spinel_t *s, bool promiscuous);

/** Clears the stored copy, then both source-match tables on the RCP (stopping at a timeout or an abort). */
void zigbee_spinel_src_match_clear(zigbee_spinel_t *s);
/**
 * Adds one entry. Nonzero when the RCP refuses it; a refused entry (> 0) is not stored, so it is never replayed. An add
 * that did not get the RCP's answer (a negative code: aborted by a RESET or GONE, timed out, or not sent) still stores
 * the entry and returns that code: the next attach replays it (and prunes it if the RCP refuses it then), so the RCP's
 * table is not left without it until the next edit.
 */
int zigbee_spinel_src_match_add_short(zigbee_spinel_t *s, uint16_t address);
int zigbee_spinel_src_match_add_ext(zigbee_spinel_t *s, const uint8_t ext[8]);

/**
 * Sends one frame (PSDU without FCS; the client appends it). Nonzero without sending when not attached, with a
 * transmit in flight, or when the frame with its FCS exceeds 127 bytes. The answer comes through sink.tx_done.
 */
int zigbee_spinel_transmit(zigbee_spinel_t *s, const uint8_t *psdu, size_t len, uint8_t channel, bool csma,
                           uint8_t retries, uint8_t backoffs);

/**
 * Starts an energy scan of one channel for `ms`: the scan's three property requests run synchronously, so a refusal
 * returns nonzero; the peak RSSI comes through sink.scan_done.
 */
int zigbee_spinel_energy_scan(zigbee_spinel_t *s, uint8_t channel, uint16_t ms);

/** One spinel frame from the channel, received at `rx_us` (the mux's time). The frame is not kept. */
void zigbee_spinel_on_frame(zigbee_spinel_t *s, const uint8_t *frame, size_t len, int64_t rx_us);

/**
 * Checks the transmit, re-arm and energy-scan deadlines; a missed one requests a channel reset. A timed-out transmit or
 * scan stays open until the RESET that answers the request completes it as failed.
 */
void zigbee_spinel_on_tick(zigbee_spinel_t *s);

/**
 * Ends the property request now waiting: the waiting call returns ZIGBEE_SPINEL_ERR_ABORTED and requests no channel
 * reset. No request waiting: nothing happens. zigbee_spinel_on_reset / _on_gone / _attach call it themselves.
 */
void zigbee_spinel_abort_wait(zigbee_spinel_t *s);

/**
 * The mux reset or reopened the radio (RESET); the client also calls it itself on an unsolicited LAST_STATUS in the
 * reset range (the RCP rebooted on its own). Counts the reset, completes a transmit in flight as failed and an energy
 * scan in progress with ZIGBEE_SPINEL_SCAN_FAILED_RSSI, then runs the attach sequence and the replay (see "Radio
 * resets and loss" above). With no radio: nothing happens.
 */
void zigbee_spinel_on_reset(zigbee_spinel_t *s);

/**
 * The mux detached the channel (GONE: unplugged, a C6 job, a mode change). Completes a transmit in flight as failed
 * and an energy scan in progress with ZIGBEE_SPINEL_SCAN_FAILED_RSSI, ends a waiting request, and leaves the client
 * with no radio until the next zigbee_spinel_attach.
 */
void zigbee_spinel_on_gone(zigbee_spinel_t *s);

/** A channel is attached: true from zigbee_spinel_attach until zigbee_spinel_on_gone. */
bool zigbee_spinel_has_radio(const zigbee_spinel_t *s);

/** RESETs handled since zigbee_spinel_init: the mux's and the RCP's unsolicited ones (not the attach's own reset). */
uint32_t zigbee_spinel_resets(const zigbee_spinel_t *s);

/** The RCP's EUI-64 as HWADDR reported it (RadioSpinel GetIeeeEui64). false before the first attach read it. */
bool zigbee_spinel_eui64(const zigbee_spinel_t *s, uint8_t out[8]);

#ifdef __cplusplus
}
#endif
