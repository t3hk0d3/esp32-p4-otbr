#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "spinel.h"
#include "zigbee_spinel.h"

/*
 * The encodings below follow OpenThread's RadioSpinel and SpinelDriver (ESP-IDF 6.1, lib/spinel); the function each
 * one mirrors is named where it is built or parsed.
 */

/* spinel.h's SPINEL_HEADER_IID() uses static_cast: plain-C header handling. The host is IID 0, as ESP-IDF's
 * esp_openthread_radio_spinel.cpp sets it; an RCP built for multipan sends notifications on the broadcast IID 3. */
#define HEADER(tid) ((uint8_t)(SPINEL_HEADER_FLAG | (tid)))
#define HEADER_IID(h) (((h) & SPINEL_HEADER_IID_MASK) >> SPINEL_HEADER_IID_SHIFT)
#define HEADER_TID(h) ((uint8_t)((h) & SPINEL_HEADER_TID_MASK))
#define IID_HOST 0
#define IID_BROADCAST 3
#define TID_MAX 15

#define FCS_LEN 2
#define RADIO_POWER_INVALID 127 // OT_RADIO_POWER_INVALID: no per-frame transmit power

/* OT_RADIO_CAPS_* (openthread/platform/radio.h). */
#define CAPS_ACK_TIMEOUT (1u << 0)
#define CAPS_ENERGY_SCAN (1u << 1)
#define CAPS_TRANSMIT_RETRIES (1u << 2)
#define CAPS_CSMA_BACKOFF (1u << 3)
#define CAPS_RX_ON_WHEN_IDLE (1u << 8)
/* The RCP does ACKs, CSMA and retries for ziggurat, and the energy scan. */
#define CAPS_REQUIRED (CAPS_ACK_TIMEOUT | CAPS_ENERGY_SCAN | CAPS_TRANSMIT_RETRIES | CAPS_CSMA_BACKOFF)

/* OT_ERROR_ABORT as NcpBase::ThreadErrorToSpinelStatus wraps it (no spinel status of its own). */
#define STATUS_ABORT (SPINEL_STATUS_STACK_NATIVE__BEGIN + 11)

#define NEVER INT64_MAX

static void run_attach(zigbee_spinel_t *s);

static int64_t now(const zigbee_spinel_t *s)
{
    return s->ops.now_us(s->ops.ctx);
}

/* A RESET, GONE or attach that arrived inside a wait ended this call's work: its requests fail without sending. */
static bool unwinding(const zigbee_spinel_t *s)
{
    return !s->radio || s->attach_pending;
}

/* Client calls that may wait nest (an event inside ops->wait); the outermost one runs a pending attach when it has
 * unwound. */
static void enter(zigbee_spinel_t *s)
{
    s->depth++;
}

static int leave(zigbee_spinel_t *s, int rc)
{
    if (--s->depth == 0 && s->attach_pending) {
        run_attach(s);
    }
    return rc;
}

/* RadioSpinel::GetNextTid: TIDs 1..15 in turn, skipping the ones in use; 0 when all are taken. */
static uint8_t take_tid(zigbee_spinel_t *s)
{
    for (int i = 0; i < TID_MAX; i++) {
        uint8_t tid = s->next_tid;

        s->next_tid = tid >= TID_MAX ? 1 : (uint8_t)(tid + 1);
        if ((s->tids_in_use & (1u << tid)) == 0) {
            s->tids_in_use |= (uint16_t)(1u << tid);
            return tid;
        }
    }
    return 0;
}

static void free_tid(zigbee_spinel_t *s, uint8_t tid)
{
    s->tids_in_use &= (uint16_t)~(1u << tid);
}

/* SpinelDriver::SendCommand: "Cii" header, command, property, then the value as `fmt`. */
static int send_propv(zigbee_spinel_t *s, uint8_t tid, unsigned cmd, unsigned key, const char *fmt, va_list args)
{
    spinel_ssize_t n = spinel_datatype_pack(s->frame, sizeof(s->frame), "Cii", HEADER(tid), cmd, key);

    if (n <= 0 || (size_t)n > sizeof(s->frame)) {
        return -1;
    }
    if (fmt != NULL) {
        spinel_ssize_t m = spinel_datatype_vpack(s->frame + n, sizeof(s->frame) - (size_t)n, fmt, args);

        if (m < 0 || (size_t)(n + m) > sizeof(s->frame)) {
            return -1;
        }
        n += m;
    }
    return s->ops.send(s->ops.ctx, s->frame, (size_t)n) == 0 ? 0 : -1;
}

static int send_prop(zigbee_spinel_t *s, uint8_t tid, unsigned cmd, unsigned key, const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    int rc = send_propv(s, tid, cmd, key, fmt, args);
    va_end(args);
    return rc;
}

/*
 * RadioSpinel::WaitResponse: lets the board deliver frames until the reply came or the request timed out (then
 * RadioSpinel::HandleRcpTimeout's counterpart: a channel reset is requested). Returns 0, the RCP's nonzero status,
 * SPINEL_STATUS_FAILURE for a reply that is not the expected one, ZIGBEE_SPINEL_ERR_TIMEOUT, or
 * ZIGBEE_SPINEL_ERR_ABORTED (no reset requested: whoever aborted handles the radio).
 */
static int wait_reply(zigbee_spinel_t *s)
{
    int64_t deadline = now(s) + ZIGBEE_SPINEL_REQUEST_TIMEOUT_US;
    int rc;

    while (s->wait_state == ZIGBEE_SPINEL_WAIT_PENDING && now(s) < deadline) {
        // The clock decides; wait's own "nothing came" result only says the same thing earlier.
        (void)s->ops.wait(s->ops.ctx, deadline);
    }
    switch (s->wait_state) {
    case ZIGBEE_SPINEL_WAIT_OK:
        rc = 0;
        break;
    case ZIGBEE_SPINEL_WAIT_FAILED:
        rc = s->reply_status != SPINEL_STATUS_OK ? (int)s->reply_status : SPINEL_STATUS_FAILURE;
        break;
    case ZIGBEE_SPINEL_WAIT_ABORTED:
        rc = ZIGBEE_SPINEL_ERR_ABORTED;
        break;
    default:
        rc = ZIGBEE_SPINEL_ERR_TIMEOUT;
        break;
    }
    if (!s->wait_for_reset) {
        free_tid(s, s->wait_tid);
    }
    s->wait_state = ZIGBEE_SPINEL_WAIT_IDLE;
    s->wait_for_reset = false;
    if (rc == ZIGBEE_SPINEL_ERR_TIMEOUT) {
        (void)s->ops.request_reset(s->ops.ctx); // refused: the call has failed already, nothing stays open
    }
    return rc;
}

/* RadioSpinel::RequestV for everything but STREAM_RAW: one request, then its reply. `expect` is the command of a
 * success reply that carries the value (RadioSpinel's mExpectedCommand); LAST_STATUS OK also succeeds. */
static int request(zigbee_spinel_t *s, unsigned cmd, unsigned key, unsigned expect, const char *fmt, ...)
{
    if (unwinding(s)) {
        return ZIGBEE_SPINEL_ERR_ABORTED;
    }
    if (s->wait_state != ZIGBEE_SPINEL_WAIT_IDLE) {
        return ZIGBEE_SPINEL_ERR_LOCAL; // one property request at a time
    }
    uint8_t tid = take_tid(s);
    if (tid == 0) {
        return ZIGBEE_SPINEL_ERR_LOCAL;
    }
    s->wait_tid = tid;
    s->wait_key = key;
    s->wait_cmd = expect;
    s->wait_for_reset = false;
    s->reply_status = SPINEL_STATUS_OK;
    s->reply_len = 0;
    s->wait_state = ZIGBEE_SPINEL_WAIT_PENDING;

    va_list args;
    va_start(args, fmt);
    int rc = send_propv(s, tid, cmd, key, fmt, args);
    va_end(args);
    if (rc != 0) {
        free_tid(s, tid);
        s->wait_state = ZIGBEE_SPINEL_WAIT_IDLE;
        return ZIGBEE_SPINEL_ERR_LOCAL;
    }
    return wait_reply(s);
}

#define GET(s, key) request((s), SPINEL_CMD_PROP_VALUE_GET, (key), SPINEL_CMD_PROP_VALUE_IS, NULL)
#define SET(s, key, ...) request((s), SPINEL_CMD_PROP_VALUE_SET, (key), SPINEL_CMD_PROP_VALUE_IS, __VA_ARGS__)
#define INSERT(s, key, ...) \
    request((s), SPINEL_CMD_PROP_VALUE_INSERT, (key), SPINEL_CMD_PROP_VALUE_INSERTED, __VA_ARGS__)

static void reversed(uint8_t out[8], const uint8_t in[8])
{
    for (int i = 0; i < 8; i++) {
        out[i] = in[7 - i];
    }
}

/* The 802.15.4 FCS: CRC-16/KERMIT (ITU-T polynomial, reflected, initial value 0), sent least significant byte
 * first. RadioSpinel sends whatever the frame's FCS bytes hold; ziggurat-phy-spinel computes it, as here. */
static uint16_t fcs(const uint8_t *data, size_t len)
{
    uint16_t crc = 0;

    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0x8408) : (uint16_t)(crc >> 1);
        }
    }
    return crc;
}

/* ---- configuration and source match ---- */

/* RadioSpinel::Receive (PHY_CHAN "C"), SetTransmitPower ("c"), SetPanId ("S"), SetShortAddress ("S"),
 * SetExtendedAddress ("E", the reversed address that Radio::SetExtendedAddress hands it), SetPromiscuous ("C",
 * NETWORK or OFF), EnableSrcMatch ("b"), SetRxOnWhenIdle ("b", only with the RX_ON_WHEN_IDLE radio cap, as in
 * RadioSpinel::RestoreProperties). Stops at the first refusal. */
static int send_config(zigbee_spinel_t *s, const zigbee_radio_config_t *c)
{
    uint8_t ext[8];
    int rc;

    reversed(ext, c->ext_addr);
    if ((rc = SET(s, SPINEL_PROP_PHY_CHAN, "C", c->channel)) != 0 ||
            (rc = SET(s, SPINEL_PROP_PHY_TX_POWER, "c", c->tx_power_dbm)) != 0 ||
            (rc = SET(s, SPINEL_PROP_MAC_15_4_PANID, "S", c->pan_id)) != 0 ||
            (rc = SET(s, SPINEL_PROP_MAC_15_4_SADDR, "S", c->short_addr)) != 0 ||
            (rc = SET(s, SPINEL_PROP_MAC_15_4_LADDR, "E", ext)) != 0 ||
            (rc = SET(s, SPINEL_PROP_MAC_PROMISCUOUS_MODE, "C",
                      c->promiscuous ? SPINEL_MAC_PROMISCUOUS_MODE_NETWORK : SPINEL_MAC_PROMISCUOUS_MODE_OFF)) != 0 ||
            (rc = SET(s, SPINEL_PROP_MAC_SRC_MATCH_ENABLED, "b", true)) != 0) {
        return rc;
    }
    if (s->radio_caps & CAPS_RX_ON_WHEN_IDLE) {
        rc = SET(s, SPINEL_PROP_MAC_RX_ON_WHEN_IDLE_MODE, "b", true);
    }
    return rc;
}

/* RadioSpinel::ClearSrcMatchShortEntries / ClearSrcMatchExtEntries: a set with no value. Stops at a timeout or an
 * abort; otherwise returns the first refusal. */
static int send_src_clear(zigbee_spinel_t *s)
{
    int rc = SET(s, SPINEL_PROP_MAC_SRC_MATCH_SHORT_ADDRESSES, NULL);

    if (rc < 0) {
        return rc;
    }
    int rc_ext = SET(s, SPINEL_PROP_MAC_SRC_MATCH_EXTENDED_ADDRESSES, NULL);
    return rc_ext < 0 || rc == 0 ? rc_ext : rc;
}

/* RadioSpinel::AddSrcMatchShortEntry: INSERT "S". */
static int send_src_short(zigbee_spinel_t *s, uint16_t address)
{
    return INSERT(s, SPINEL_PROP_MAC_SRC_MATCH_SHORT_ADDRESSES, "S", address);
}

/* RadioSpinel::AddSrcMatchExtEntry: INSERT "E" of the address Radio::AddSrcMatchExtEntry reversed. */
static int send_src_ext(zigbee_spinel_t *s, const uint8_t ext[8])
{
    uint8_t rev[8];

    reversed(rev, ext);
    return INSERT(s, SPINEL_PROP_MAC_SRC_MATCH_EXTENDED_ADDRESSES, "E", rev);
}

static bool has_short(const zigbee_spinel_t *s, uint16_t address)
{
    for (size_t i = 0; i < s->src_short_count; i++) {
        if (s->src_short[i] == address) {
            return true;
        }
    }
    return false;
}

static bool has_ext(const zigbee_spinel_t *s, const uint8_t ext[8])
{
    for (size_t i = 0; i < s->src_ext_count; i++) {
        if (memcmp(s->src_ext[i], ext, 8) == 0) {
            return true;
        }
    }
    return false;
}

/* RadioSpinel::RestoreProperties for the tables: clear both, insert what is stored. Entries the RCP refuses (> 0)
 * leave the stored copy once the whole replay has run; a timeout or an abort stops the replay and leaves the stored
 * copy as it was. */
static int replay_src_match(zigbee_spinel_t *s)
{
    bool refused_short[ZIGBEE_SPINEL_SRC_MATCH_MAX] = {false};
    bool refused_ext[ZIGBEE_SPINEL_SRC_MATCH_MAX] = {false};
    size_t kept = 0;
    int rc = send_src_clear(s);

    if (rc < 0) {
        return rc;
    }
    for (size_t i = 0; i < s->src_short_count; i++) {
        if ((rc = send_src_short(s, s->src_short[i])) < 0) {
            return rc;
        }
        refused_short[i] = rc != 0;
    }
    for (size_t i = 0; i < s->src_ext_count; i++) {
        if ((rc = send_src_ext(s, s->src_ext[i])) < 0) {
            return rc;
        }
        refused_ext[i] = rc != 0;
    }
    for (size_t i = 0; i < s->src_short_count; i++) {
        if (!refused_short[i]) {
            s->src_short[kept++] = s->src_short[i];
        }
    }
    s->src_short_count = kept;
    kept = 0;
    for (size_t i = 0; i < s->src_ext_count; i++) {
        if (!refused_ext[i]) {
            memmove(s->src_ext[kept++], s->src_ext[i], 8);
        }
    }
    s->src_ext_count = kept;
    return 0;
}

/* ---- attach ---- */

static void incompatible(zigbee_spinel_t *s, const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    vsnprintf(s->reason, sizeof(s->reason), fmt, args);
    va_end(args);
    s->sink.ready(s->sink.ctx, false, s->reason);
}

/* SpinelDriver::ResetCoprocessor (software reset): CMD_RESET STACK on TID 0, answered by a LAST_STATUS in the reset
 * range as a notification. */
static int reset_rcp(zigbee_spinel_t *s)
{
    spinel_ssize_t n = spinel_datatype_pack(s->frame, sizeof(s->frame), SPINEL_DATATYPE_COMMAND_S "C", HEADER(0),
                                            SPINEL_CMD_RESET, SPINEL_RESET_STACK);

    if (n <= 0 || (size_t)n > sizeof(s->frame) || s->wait_state != ZIGBEE_SPINEL_WAIT_IDLE) {
        return ZIGBEE_SPINEL_ERR_LOCAL;
    }
    s->wait_for_reset = true;
    s->reply_status = SPINEL_STATUS_OK;
    s->wait_state = ZIGBEE_SPINEL_WAIT_PENDING;
    if (s->ops.send(s->ops.ctx, s->frame, (size_t)n) != 0) {
        s->wait_for_reset = false;
        s->wait_state = ZIGBEE_SPINEL_WAIT_IDLE;
        return ZIGBEE_SPINEL_ERR_LOCAL;
    }
    return wait_reply(s);
}

/* SpinelDriver::GetCoprocessorCaps + RadioSpinel::InitializeCaps: the CAPS list must hold CONFIG_RADIO and MAC_RAW. */
static bool is_rcp(const uint8_t *caps, size_t len)
{
    bool radio = false, raw = false;

    while (len > 0) {
        unsigned cap = 0;
        spinel_ssize_t n = spinel_packed_uint_decode(caps, len, &cap);

        if (n <= 0 || (size_t)n > len) {
            return false;
        }
        radio |= cap == SPINEL_CAP_CONFIG_RADIO;
        raw |= cap == SPINEL_CAP_MAC_RAW;
        caps += n;
        len -= (size_t)n;
    }
    return radio && raw;
}

/* RadioSpinel::CheckRadioCapabilities' names, by bit. */
static void missing_caps(char *out, size_t size, unsigned missing)
{
    static const char *const names[] = {"ack-timeout", "energy-scan", "tx-retries", "CSMA-backoff"};
    size_t len = 0;

    out[0] = '\0';
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if ((missing & (1u << i)) && len < size) {
            len += (size_t)snprintf(out + len, size - len, "%s%s", len ? " " : "", names[i]);
        }
    }
}

/*
 * What the radio's reset or loss leaves open ends here, each through the sink once: the transmit in flight fails, the
 * energy scan in progress reports ZIGBEE_SPINEL_SCAN_FAILED_RSSI (one still being started is energy_scan's to fail),
 * a pending re-arm is forgotten and every TID is free again (RadioSpinel::RecoverFromRcpFailure clears its TIDs too).
 */
static void drop_open(zigbee_spinel_t *s)
{
    bool tx = s->tx_in_flight;
    bool scan = s->scanning && !s->scan_starting;

    s->tx_in_flight = false;
    s->tx_tid = 0;
    s->tx_deadline_us = NEVER;
    s->rearm_tid = 0;
    s->rearm_deadline_us = NEVER;
    if (scan) {
        s->scanning = false;
        s->scan_deadline_us = NEVER;
    }
    s->tids_in_use = 0;
    if (tx) {
        s->sink.tx_done(s->sink.ctx, ZIGBEE_TX_FAILED);
    }
    if (scan) {
        s->sink.scan_done(s->sink.ctx, ZIGBEE_SPINEL_SCAN_FAILED_RSSI);
    }
}

/* The attach sequence (zigbee_spinel_attach's comment). Stops quietly when a request times out or is aborted. */
static void attach_sequence(zigbee_spinel_t *s)
{
    unsigned major = 0, minor = 0, radio_caps = 0;
    int rc;

    s->attached = false;
    drop_open(s);
    if (reset_rcp(s) != 0) {
        return;
    }

    // SpinelDriver::CheckSpinelVersion: "ii". Only the major version is checked.
    if ((rc = GET(s, SPINEL_PROP_PROTOCOL_VERSION)) != 0) {
        if (rc > 0) {
            incompatible(s, "PROTOCOL_VERSION refused (status %d)", rc);
        }
        return;
    }
    if (spinel_datatype_unpack(s->reply, s->reply_len, "ii", &major, &minor) <= 0) {
        incompatible(s, "PROTOCOL_VERSION unreadable");
        return;
    }
    if (major != ZIGBEE_SPINEL_PROTOCOL_MAJOR) {
        incompatible(s, "spinel protocol %u.%u, need %d.x", major, minor, ZIGBEE_SPINEL_PROTOCOL_MAJOR);
        return;
    }

    // SpinelDriver::GetCoprocessorVersion: "U" (read, as RadioSpinel does; not used).
    if ((rc = GET(s, SPINEL_PROP_NCP_VERSION)) != 0) {
        if (rc > 0) {
            incompatible(s, "NCP_VERSION refused (status %d)", rc);
        }
        return;
    }

    if ((rc = GET(s, SPINEL_PROP_CAPS)) != 0) {
        if (rc > 0) {
            incompatible(s, "CAPS refused (status %d)", rc);
        }
        return;
    }
    if (!is_rcp(s->reply, s->reply_len)) {
        incompatible(s, "not an RCP (no CONFIG_RADIO and MAC_RAW caps)");
        return;
    }

    // RadioSpinel::CheckRadioCapabilities: "i".
    if ((rc = GET(s, SPINEL_PROP_RADIO_CAPS)) != 0) {
        if (rc > 0) {
            incompatible(s, "RADIO_CAPS refused (status %d)", rc);
        }
        return;
    }
    if (spinel_datatype_unpack(s->reply, s->reply_len, "i", &radio_caps) <= 0) {
        incompatible(s, "RADIO_CAPS unreadable");
        return;
    }
    s->radio_caps = radio_caps;
    if ((radio_caps & CAPS_REQUIRED) != CAPS_REQUIRED) {
        char names[64];

        missing_caps(names, sizeof(names), CAPS_REQUIRED & ~radio_caps);
        incompatible(s, "RCP lacks radio caps: %s", names);
        return;
    }

    // RadioSpinel::Init: HWADDR "E".
    if ((rc = GET(s, SPINEL_PROP_HWADDR)) != 0) {
        if (rc > 0) {
            incompatible(s, "HWADDR refused (status %d)", rc);
        }
        return;
    }
    if (spinel_datatype_unpack_in_place(s->reply, s->reply_len, "E", s->eui64) <= 0) {
        incompatible(s, "HWADDR unreadable");
        return;
    }
    s->have_eui64 = true;

    // RadioSpinel::Enable (PHY_ENABLED "b") and Receive from sleep (MAC_RAW_STREAM_ENABLED "b").
    if ((rc = SET(s, SPINEL_PROP_PHY_ENABLED, "b", true)) != 0 ||
            (rc = SET(s, SPINEL_PROP_MAC_RAW_STREAM_ENABLED, "b", true)) != 0) {
        if (rc > 0) {
            incompatible(s, "radio enable refused (status %d)", rc);
        }
        return;
    }
    s->attached = true;

    // RadioSpinel::RestoreProperties' counterpart. A refused property leaves the radio attached (the next edit
    // retries); a timeout or an abort stops here without ready.
    if (s->have_config && (rc = send_config(s, &s->config)) < 0) {
        return;
    }
    if ((s->src_short_count > 0 || s->src_ext_count > 0) && replay_src_match(s) < 0) {
        return;
    }
    s->sink.ready(s->sink.ctx, true, NULL);
}

/*
 * Runs the attach sequence while one is pending. A RESET (or an attach) inside one of its waits aborts that wait,
 * which stops the sequence, and sets attach_pending again: the loop then restarts it from CMD_RESET. A loop, not a
 * recursion, so a reset storm costs one sequence per RESET and never stack. GONE clears both flags and ends it.
 */
static void run_attach(zigbee_spinel_t *s)
{
    s->depth++;
    while (s->attach_pending && s->radio) {
        s->attach_pending = false;
        attach_sequence(s);
    }
    s->depth--;
}

void zigbee_spinel_attach(zigbee_spinel_t *s)
{
    s->radio = true;
    s->attach_pending = true;
    if (s->depth > 0) {
        zigbee_spinel_abort_wait(s); // the outermost call runs it once it has unwound
        return;
    }
    run_attach(s);
}

void zigbee_spinel_on_reset(zigbee_spinel_t *s)
{
    if (!s->radio) {
        return; // GONE came first: the next attach brings the radio back
    }
    s->resets++;
    s->attached = false;
    drop_open(s);
    s->attach_pending = true;
    zigbee_spinel_abort_wait(s);
    if (s->depth == 0) {
        run_attach(s);
    }
}

void zigbee_spinel_on_gone(zigbee_spinel_t *s)
{
    s->radio = false;
    s->attached = false;
    s->attach_pending = false;
    drop_open(s);
    zigbee_spinel_abort_wait(s);
}

bool zigbee_spinel_has_radio(const zigbee_spinel_t *s)
{
    return s->radio;
}

uint32_t zigbee_spinel_resets(const zigbee_spinel_t *s)
{
    return s->resets;
}

/* ---- public operations ---- */

void zigbee_spinel_init(zigbee_spinel_t *s, const zigbee_spinel_ops_t *ops, const zigbee_spinel_sink_t *sink)
{
    memset(s, 0, sizeof(*s));
    s->ops = *ops;
    s->sink = *sink;
    s->next_tid = 1;
    s->wait_state = ZIGBEE_SPINEL_WAIT_IDLE;
    s->tx_deadline_us = NEVER;
    s->rearm_deadline_us = NEVER;
    s->scan_deadline_us = NEVER;
}

/* With no radio (or before the attach sequence enabled it) these store the change for the replay and return 0. */
int zigbee_spinel_configure(zigbee_spinel_t *s, const zigbee_radio_config_t *config)
{
    enter(s);
    s->config = *config;
    s->have_config = true;
    return leave(s, s->attached ? send_config(s, &s->config) : 0);
}

int zigbee_spinel_set_channel(zigbee_spinel_t *s, uint8_t channel)
{
    enter(s);
    s->config.channel = channel;
    return leave(s, s->attached ? SET(s, SPINEL_PROP_PHY_CHAN, "C", channel) : 0);
}

int zigbee_spinel_set_promiscuous(zigbee_spinel_t *s, bool promiscuous)
{
    enter(s);
    s->config.promiscuous = promiscuous;
    if (!s->attached) {
        return leave(s, 0);
    }
    return leave(s, SET(s, SPINEL_PROP_MAC_PROMISCUOUS_MODE, "C",
                        promiscuous ? SPINEL_MAC_PROMISCUOUS_MODE_NETWORK : SPINEL_MAC_PROMISCUOUS_MODE_OFF));
}

void zigbee_spinel_src_match_clear(zigbee_spinel_t *s)
{
    enter(s);
    s->src_short_count = 0;
    s->src_ext_count = 0;
    if (s->attached) {
        (void)send_src_clear(s);
    }
    (void)leave(s, 0);
}

int zigbee_spinel_src_match_add_short(zigbee_spinel_t *s, uint16_t address)
{
    bool stored = has_short(s, address);

    if (!stored && s->src_short_count >= ZIGBEE_SPINEL_SRC_MATCH_MAX) {
        return ZIGBEE_SPINEL_ERR_LOCAL;
    }
    enter(s);
    int rc = s->attached ? send_src_short(s, address) : 0;

    // A refusal (> 0) is the RCP's answer: not stored. Without an answer the entry is stored before leave(), whose
    // attach (after a RESET) replays it at once.
    if (!stored && rc <= 0) {
        s->src_short[s->src_short_count++] = address;
    }
    return leave(s, rc);
}

int zigbee_spinel_src_match_add_ext(zigbee_spinel_t *s, const uint8_t ext[8])
{
    bool stored = has_ext(s, ext);

    if (!stored && s->src_ext_count >= ZIGBEE_SPINEL_SRC_MATCH_MAX) {
        return ZIGBEE_SPINEL_ERR_LOCAL;
    }
    enter(s);
    int rc = s->attached ? send_src_ext(s, ext) : 0;

    // A refusal (> 0) is the RCP's answer: not stored. Without an answer the entry is stored before leave(), whose
    // attach (after a RESET) replays it at once.
    if (!stored && rc <= 0) {
        memcpy(s->src_ext[s->src_ext_count++], ext, 8);
    }
    return leave(s, rc);
}

/*
 * RadioSpinel::Transmit: STREAM_RAW "d C C C b b b b L L C c" (frame with its FCS, channel, max CSMA backoffs, max
 * frame retries, CSMA on, header updated, retransmission, security processed, TX delay, delay base time, receive
 * channel after the transmit, per-frame power). Ziggurat's frames are secured and final (the spike's glue set
 * mIsSecurityProcessed and mIsHeaderUpdated). Not waited for: the answer is the LAST_STATUS on its TID.
 */
int zigbee_spinel_transmit(zigbee_spinel_t *s, const uint8_t *psdu, size_t len, uint8_t channel, bool csma,
                           uint8_t retries, uint8_t backoffs)
{
    if (!s->attached || s->tx_in_flight || psdu == NULL || len + FCS_LEN > ZIGBEE_SPINEL_PSDU_MAX) {
        return ZIGBEE_SPINEL_ERR_LOCAL;
    }
    uint8_t tid = take_tid(s);
    if (tid == 0) {
        return ZIGBEE_SPINEL_ERR_LOCAL;
    }
    uint16_t crc = fcs(psdu, len);

    memcpy(s->psdu, psdu, len);
    s->psdu[len] = (uint8_t)(crc & 0xff);
    s->psdu[len + 1] = (uint8_t)(crc >> 8);
    if (send_prop(s, tid, SPINEL_CMD_PROP_VALUE_SET, SPINEL_PROP_STREAM_RAW, "dCCCbbbbLLCc", s->psdu,
                  (uint32_t)(len + FCS_LEN), channel, backoffs, retries, csma, true, false, true, (uint32_t)0,
                  (uint32_t)0, channel, RADIO_POWER_INVALID) != 0) {
        free_tid(s, tid);
        return ZIGBEE_SPINEL_ERR_LOCAL;
    }
    s->tx_in_flight = true;
    s->tx_tid = tid;
    s->tx_deadline_us = now(s) + ZIGBEE_SPINEL_TX_TIMEOUT_US;
    return 0;
}

/* RadioSpinel::EnergyScan: MAC_SCAN_MASK "D" (one channel byte), MAC_SCAN_PERIOD "S", MAC_SCAN_STATE "C" ENERGY. */
int zigbee_spinel_energy_scan(zigbee_spinel_t *s, uint8_t channel, uint16_t ms)
{
    int rc;

    if (!s->attached || s->scanning) {
        return ZIGBEE_SPINEL_ERR_LOCAL;
    }
    enter(s);
    if ((rc = SET(s, SPINEL_PROP_MAC_SCAN_MASK, "D", &channel, (uint32_t)1)) != 0 ||
            (rc = SET(s, SPINEL_PROP_MAC_SCAN_PERIOD, "S", ms)) != 0) {
        return leave(s, rc);
    }
    // Scanning before the last request: its result may arrive inside that request's wait.
    s->scanning = true;
    s->scan_starting = true;
    s->scan_deadline_us = NEVER;
    rc = SET(s, SPINEL_PROP_MAC_SCAN_STATE, "C", SPINEL_SCAN_STATE_ENERGY);
    s->scan_starting = false;
    if (rc == 0 && s->scanning && unwinding(s)) {
        rc = ZIGBEE_SPINEL_ERR_ABORTED; // the radio was reset or lost before any result: this call fails it
    }
    if (rc != 0) {
        s->scanning = false;
        return leave(s, rc);
    }
    if (s->scanning) {
        s->scan_deadline_us = now(s) + (int64_t)ms * 1000 + ZIGBEE_SPINEL_SCAN_GRACE_US;
    }
    return leave(s, 0);
}

/* ---- received frames ---- */

/* RadioSpinel::HandleWaitingResponse: LAST_STATUS carries the status; the waited-for property with the expected
 * command carries the value; anything else is a failure (OT_ERROR_DROP). */
static void handle_waiting_reply(zigbee_spinel_t *s, unsigned cmd, unsigned key, const uint8_t *data, size_t len)
{
    if (key == SPINEL_PROP_LAST_STATUS) {
        unsigned status = SPINEL_STATUS_FAILURE;

        if (spinel_datatype_unpack(data, len, "i", &status) <= 0) {
            status = SPINEL_STATUS_PARSE_ERROR;
        }
        s->reply_status = status;
        s->wait_state = status == SPINEL_STATUS_OK ? ZIGBEE_SPINEL_WAIT_OK : ZIGBEE_SPINEL_WAIT_FAILED;
    } else if (key == s->wait_key && cmd == s->wait_cmd && len <= sizeof(s->reply)) {
        memcpy(s->reply, data, len);
        s->reply_len = len;
        s->wait_state = ZIGBEE_SPINEL_WAIT_OK;
    } else {
        s->wait_state = ZIGBEE_SPINEL_WAIT_FAILED;
    }
}

/* SpinelStatusToOtError, then the spike's tx_done mapping onto ziggurat's results. */
static zigbee_tx_result_t tx_result(unsigned status)
{
    switch (status) {
    case SPINEL_STATUS_OK:
        return ZIGBEE_TX_ACKED;
    case SPINEL_STATUS_NO_ACK:
        return ZIGBEE_TX_NO_ACK;
    case SPINEL_STATUS_CCA_FAILURE:
        return ZIGBEE_TX_CCA;
    case STATUS_ABORT:
        return ZIGBEE_TX_ABORTED;
    default:
        return ZIGBEE_TX_FAILED;
    }
}

/*
 * RadioSpinel::HandleTransmitDone: LAST_STATUS "i b b [ACK frame]". Only the status is used; an immediate refusal
 * (NcpBase::HandlePropertySet_SPINEL_PROP_STREAM_RAW) carries the status alone. On an RCP without rx-on-when-idle
 * the receiver is put back on (RadioSpinel::Receive from sleep: MAC_RAW_STREAM_ENABLED), without waiting.
 */
static void handle_tx_answer(zigbee_spinel_t *s, unsigned cmd, unsigned key, const uint8_t *data, size_t len)
{
    zigbee_tx_result_t r = ZIGBEE_TX_FAILED;
    unsigned status = 0;

    if (cmd == SPINEL_CMD_PROP_VALUE_IS && key == SPINEL_PROP_LAST_STATUS &&
            spinel_datatype_unpack(data, len, "i", &status) > 0) {
        r = tx_result(status);
    }
    free_tid(s, s->tx_tid);
    s->tx_tid = 0;
    s->tx_in_flight = false;
    s->tx_deadline_us = NEVER;

    if (!(s->radio_caps & CAPS_RX_ON_WHEN_IDLE) && s->rearm_tid == 0) {
        uint8_t tid = take_tid(s);

        if (tid != 0) {
            if (send_prop(s, tid, SPINEL_CMD_PROP_VALUE_SET, SPINEL_PROP_MAC_RAW_STREAM_ENABLED, "b", true) == 0) {
                s->rearm_tid = tid;
                s->rearm_deadline_us = now(s) + ZIGBEE_SPINEL_REQUEST_TIMEOUT_US;
            } else {
                free_tid(s, tid);
            }
        }
    }
    s->sink.tx_done(s->sink.ctx, r);
}

/* RadioSpinel::ParseRadioFrame: "d c c S t(C C X) t(i)" (frame with its FCS, RSSI, noise floor, flags, channel, LQI,
 * RCP timestamp, receive error). The PSDU goes up without its FCS and with the mux's receive time. */
static void handle_stream_raw(zigbee_spinel_t *s, const uint8_t *data, size_t len, int64_t rx_us)
{
    const uint8_t *psdu = NULL;
    unsigned psdu_len = 0, rx_error = 0;
    int8_t rssi = 0, noise = 0;
    uint16_t flags = 0;
    uint8_t channel = 0, lqi = 0;
    uint64_t timestamp = 0;

    if (!s->attached ||
            spinel_datatype_unpack(data, len, "dccSt(CCX)t(i)", &psdu, &psdu_len, &rssi, &noise, &flags, &channel,
                                   &lqi, &timestamp, &rx_error) <= 0 ||
            rx_error != 0 || psdu_len < FCS_LEN || psdu_len > ZIGBEE_SPINEL_PSDU_MAX) {
        return;
    }
    s->sink.rx(s->sink.ctx, psdu, psdu_len - FCS_LEN, channel, rssi, lqi, (uint64_t)rx_us);
}

/* RadioSpinel::HandleNotification / HandleValueIs for TID 0. */
static void handle_notification(zigbee_spinel_t *s, unsigned cmd, unsigned key, const uint8_t *data, size_t len,
                                int64_t rx_us)
{
    if (cmd != SPINEL_CMD_PROP_VALUE_IS) {
        return;
    }
    if (key == SPINEL_PROP_STREAM_RAW) {
        handle_stream_raw(s, data, len, rx_us);
    } else if (key == SPINEL_PROP_LAST_STATUS) {
        unsigned status = 0;

        if (spinel_datatype_unpack(data, len, "i", &status) <= 0 || status < SPINEL_STATUS_RESET__BEGIN ||
                status > SPINEL_STATUS_RESET__END) {
            return;
        }
        if (s->wait_state == ZIGBEE_SPINEL_WAIT_PENDING && s->wait_for_reset) {
            s->wait_state = ZIGBEE_SPINEL_WAIT_OK; // the answer to the attach's CMD_RESET
        } else {
            zigbee_spinel_on_reset(s); // the RCP rebooted on its own (RadioSpinel::HandleRcpUnexpectedReset)
        }
    } else if (key == SPINEL_PROP_MAC_ENERGY_SCAN_RESULT) {
        uint8_t channel = 0;
        int8_t max_rssi = 0;

        if (s->scanning && spinel_datatype_unpack(data, len, "Cc", &channel, &max_rssi) > 0) {
            s->scanning = false;
            s->scan_deadline_us = NEVER;
            s->sink.scan_done(s->sink.ctx, max_rssi);
        }
    }
}

/* SpinelDriver::HandleReceivedFrame + RadioSpinel::HandleReceivedFrame / HandleResponse. */
void zigbee_spinel_on_frame(zigbee_spinel_t *s, const uint8_t *frame, size_t len, int64_t rx_us)
{
    uint8_t header = 0;
    unsigned cmd = 0, key = 0;
    const uint8_t *data = NULL;
    unsigned data_len = 0;

    if (frame == NULL || len == 0 ||
            spinel_datatype_unpack(frame, len, "CiiD", &header, &cmd, &key, &data, &data_len) <= 0 ||
            (header & SPINEL_HEADER_FLAG) != SPINEL_HEADER_FLAG ||
            (HEADER_IID(header) != IID_HOST && HEADER_IID(header) != IID_BROADCAST)) {
        return;
    }
    uint8_t tid = HEADER_TID(header);

    if (tid == 0) {
        handle_notification(s, cmd, key, data, data_len, rx_us);
    } else if (cmd < SPINEL_CMD_PROP_VALUE_IS || cmd > SPINEL_CMD_PROP_VALUE_REMOVED) {
        return; // HandleResponse's command range
    } else if (s->wait_state == ZIGBEE_SPINEL_WAIT_PENDING && !s->wait_for_reset && tid == s->wait_tid) {
        handle_waiting_reply(s, cmd, key, data, data_len);
    } else if (s->tx_in_flight && tid == s->tx_tid) {
        handle_tx_answer(s, cmd, key, data, data_len);
    } else if (s->rearm_tid != 0 && tid == s->rearm_tid) {
        free_tid(s, s->rearm_tid);
        s->rearm_tid = 0;
        s->rearm_deadline_us = NEVER;
    }
}

void zigbee_spinel_on_tick(zigbee_spinel_t *s)
{
    int64_t t = now(s);

    // A timed-out transmit or scan stays open until the requested RESET fails it. When the channel cannot be reset now
    // (the mux refuses, e.g. during a C6 firmware job) no RESET comes, so it fails here: the next one can go.
    if (s->tx_in_flight && t >= s->tx_deadline_us) {
        s->tx_deadline_us = NEVER;
        if (s->ops.request_reset(s->ops.ctx) != 0) {
            free_tid(s, s->tx_tid);
            s->tx_tid = 0;
            s->tx_in_flight = false;
            s->sink.tx_done(s->sink.ctx, ZIGBEE_TX_FAILED);
        }
    }
    if (s->rearm_tid != 0 && t >= s->rearm_deadline_us) {
        free_tid(s, s->rearm_tid);
        s->rearm_tid = 0;
        s->rearm_deadline_us = NEVER;
        (void)s->ops.request_reset(s->ops.ctx);
    }
    if (s->scanning && t >= s->scan_deadline_us) {
        s->scan_deadline_us = NEVER;
        if (s->ops.request_reset(s->ops.ctx) != 0) {
            s->scanning = false;
            s->sink.scan_done(s->sink.ctx, ZIGBEE_SPINEL_SCAN_FAILED_RSSI);
        }
    }
}

void zigbee_spinel_abort_wait(zigbee_spinel_t *s)
{
    if (s->wait_state == ZIGBEE_SPINEL_WAIT_PENDING) {
        s->wait_state = ZIGBEE_SPINEL_WAIT_ABORTED;
    }
}

bool zigbee_spinel_eui64(const zigbee_spinel_t *s, uint8_t out[8])
{
    if (!s->have_eui64) {
        return false;
    }
    memcpy(out, s->eui64, 8);
    return true;
}
