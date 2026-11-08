// Decisions of the channel/mode orchestrator. Pure: no ESP-IDF calls.
//
// The orchestrator binds mode instances (Thread, Zigbee, one RCP proxy per channel) to channels. A channel has its own
// lifecycle (channel_state); maintenance of its radio is the `reinit` state, entered only once the bound instance let
// go (the reinit run below).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "radio_mux_core.h"

#ifdef __cplusplus
extern "C" {
#endif

// A channel's lifecycle state (design section 4.1).
typedef enum {
    RADIO_MUX_CS_ABSENT, // no device: an unplugged dongle (the built-in RCP never is)
    RADIO_MUX_CS_REINIT, // maintenance runs on it: no mode may use it (with the work as its reason)
    RADIO_MUX_CS_READY,  // open and usable: a mode may be bound and run on it
    RADIO_MUX_CS_FAULT,  // unusable, with a reason and a fault kind
} radio_mux_chan_state_t;

// "absent", "reinit", "ready", "fault"; NULL for anything else.
const char *radio_mux_chan_state_name(radio_mux_chan_state_t s);

// What a channel fault is: firmware (what the built-in RCP's maintenance found: no RCP firmware, the factory
// ESP-Hosted image, a failed ROM recovery) or link (the connection failed: USB close failed, fruitless resets, no
// driver). The board shows them differently (design section 6).
typedef enum {
    RADIO_MUX_FAULT_NONE,
    RADIO_MUX_FAULT_FIRMWARE,
    RADIO_MUX_FAULT_LINK,
} radio_mux_fault_kind_t;

// The maintenance a channel in reinit runs.
typedef enum {
    RADIO_MUX_WORK_NONE,
    RADIO_MUX_WORK_BOOT_STEPS,   // the built-in RCP's boot steps
    RADIO_MUX_WORK_DETECT,       // a dongle's firmware detection after plug-in
    RADIO_MUX_WORK_PUSH,         // a firmware push to the built-in RCP
    RADIO_MUX_WORK_ROM_RECOVERY, // the built-in RCP's ROM recovery
} radio_mux_work_t;

// The reinit reason: "boot steps", "detecting firmware", "firmware push", "ROM recovery"; "" for NONE or anything
// else.
const char *radio_mux_work_reason(radio_mux_work_t w);

// The channel's busy text as the API showed it before the orchestrator: "the C6's boot steps run" for BOOT_STEPS, "a
// C6 job holds the radio" for PUSH and ROM_RECOVERY; "" otherwise.
const char *radio_mux_busy_reason(radio_mux_work_t w);

// Decision for setting channel `target` (index into chans[0..n)) to `want`: the former radio_mux_check_set_mode, plus
// a channel in reinit (in_reinit) refusing every change with RADIO_MUX_REFUSE_BUSY. Checks run in this order: bad
// value, reinit, unavailable channel, mode not registered, firmware fit, hybrid (otbr next to a stored ziggurat only
// while a ziggurat consumer is registered), move or limit, start, reboot, stop.
//
// RADIO_MUX_MOVE is every change of where OpenThread runs, each run by br_core as a long operation: otbr while another
// channel has it (a move; *move_from is that channel's index), otbr while no channel has it (a start; *move_from is
// n), and the otbr channel leaving otbr for another mode without a reboot (a stop; *move_from is target). move_from may
// be NULL. RADIO_MUX_APPLY_REBOOT: entering or leaving a mode whose rule has reboot_on_change.
radio_mux_verdict_t radio_mux_orch_check_bind(const radio_mux_rules_t *rules, const radio_mux_chan_view_t *chans,
                                              size_t n, size_t target, radio_mux_mode_t want, bool in_reinit,
                                              size_t *move_from);

// Whether the orchestrator binds a channel's configured instance now: the channel is ready, its firmware fits the
// instance's mode, and the instance is not bound to it yet.
bool radio_mux_orch_should_bind(radio_mux_chan_state_t cs, bool fits, bool bound);

// The singletons (otbr, ziggurat) run on one channel each: of stored modes in slot order (the built-in RCP first), the
// first of each stays and every later one becomes off (changed[i] set for it, cleared for the others). Returns how
// many changed. Repairs a move whose two stores were cut apart (a reset between them).
size_t radio_mux_singleton_dedupe(radio_mux_mode_t *modes, bool *changed, size_t n);

/* ---- inputs: what the orchestrator tells an instance ---- */

// The inputs an instance's state machine handles (design section 4.2). NONE is never delivered: it is the "nothing to
// deliver" answer of radio_mux_orch_input_for.
typedef enum {
    RADIO_MUX_IN_NONE,
    RADIO_MUX_IN_BIND,   // the instance may start on the channel (it is ready); a new handle
    RADIO_MUX_IN_UNBIND, // the configuration took the channel away: stop and let go
    RADIO_MUX_IN_REINIT, // maintenance wants the channel: let go (radio_mux_reinit_reply) or refuse
    RADIO_MUX_IN_LOST,   // the channel went absent or into fault while bound; the binding is kept
    RADIO_MUX_IN_BACK,   // the channel is ready again, binding unchanged: start again (a new handle)
} radio_mux_input_type_t;

// A consumer's handle on a channel: one attach or lease (radio_mux.h says how to use it).
typedef struct radio_mux_handle radio_mux_chan_t;

#define RADIO_MUX_REASON_MAX 96

// One input from the orchestrator to an instance (radio_mux_input_type_t: BIND, UNBIND, REINIT, LOST, BACK).
typedef struct {
    radio_mux_input_type_t type;
    // Grows by 1 with every input to the instance (to each channel's own instance for a per_channel one): the latest
    // input wins, and a gap tells the instance it missed one.
    uint32_t seq;
    // BIND and BACK: the new handle (a new generation). Otherwise the handle the instance holds (NULL when it holds
    // none). REINIT is answered through it (radio_mux_reinit_reply).
    radio_mux_chan_t *ch;
    radio_mux_fault_kind_t kind;      // LOST: firmware or link fault, NONE for an unplug; else NONE
    char reason[RADIO_MUX_REASON_MAX]; // LOST: the channel's reason; REINIT: the work ("firmware push"); else ""
} radio_mux_input_t;

// What a channel's change from `before` to `after` delivers to the instance of its stored mode. `fits`: the channel's
// firmware fits the mode; `bound`: the instance holds the binding (it was sent BIND and no UNBIND since; a LOST keeps
// it). A ready channel binds an instance that does not hold it (BIND: boot, plug-in, a user's bind) and gives a held
// one back after reinit, absence or a fault (BACK); one whose firmware no longer fits gets nothing (the binding stays;
// the orchestrator shows no_radio with the fit reason). A held binding whose channel goes absent or into fault from
// ready or reinit is LOST. Entering reinit delivers nothing: maintenance asks with REINIT itself.
radio_mux_input_type_t radio_mux_orch_input_for(radio_mux_chan_state_t before, radio_mux_chan_state_t after, bool fits,
                                                bool bound);

// The state the orchestrator shows for `mode`'s instance when it delivers LOST (the instance may hold no handle to
// report through: a channel that faulted before its first bind): Thread's "fault" for a firmware fault (it enters
// RADIO_FAULT), "no_radio" for anything else. The instance's own report replaces it when it has a handle.
const char *radio_mux_orch_lost_state(radio_mux_mode_t mode, radio_mux_fault_kind_t kind);

// Whether the mode stored on a channel runs again after a maintenance of it (radio_mux_maintain waits for it).
typedef enum {
    RADIO_MUX_SETTLE_NONE, // no instance is stored on the channel: nothing to wait for
    RADIO_MUX_SETTLE_WAIT, // not yet: the channel is still in reinit, or the instance has not said how it went
    RADIO_MUX_SETTLE_UP,   // the instance runs on the channel
    RADIO_MUX_SETTLE_DOWN, // it does not: the channel is absent or in fault, or the instance reports it is not running
} radio_mux_settle_t;

// The decision on a channel in state `cs`, `instance` whether its stored mode has a registered instance, `mode_state`
// what that instance (or the orchestrator for it) reports. Waits on "", "starting" and "releasing"; up on the states
// in which a mode runs (Thread's running and sniffer, Zigbee's running, the proxy's listening, connecting, backoff
// and connected); every other state is down.
radio_mux_settle_t radio_mux_orch_settle(radio_mux_chan_state_t cs, bool instance, const char *mode_state);

/* ---- the reinit run: the bound instance is asked to let go before maintenance runs ---- */

// How long the orchestrator waits for the bound instance's answer: Thread's 45 s session drain plus margin. No answer
// by then is a refusal.
#define RADIO_MUX_REINIT_REPLY_MS 50000

typedef enum {
    RADIO_MUX_RP_IDLE,    // no maintenance
    RADIO_MUX_RP_ASKED,   // the bound instance was asked to let go; waiting for its answer
    RADIO_MUX_RP_RUNNING, // the instance let go (or none was bound): the work runs, the channel is in reinit
} radio_mux_reinit_phase_t;

typedef enum {
    RADIO_MUX_RA_WAIT,   // keep waiting for the answer
    RADIO_MUX_RA_RUN,    // run the work now
    RADIO_MUX_RA_REFUSE, // refused: the instance's refusal, a timeout, or another run in progress; nothing runs
    RADIO_MUX_RA_IGNORE, // nothing to do (a stale answer, a late refusal, a tick with nothing asked)
    RADIO_MUX_RA_REBIND, // the instance let go after the run timed out: bind it back (no maintenance runs)
} radio_mux_reinit_act_t;

typedef struct {
    radio_mux_reinit_phase_t phase;
    uint32_t seq;        // the seq of the REINIT input the instance was sent
    int64_t deadline_ms; // ASKED: when no answer counts as a refusal
    bool timed_out;      // IDLE: the last run (seq) ended by its deadline; its instance's accept is still to come
} radio_mux_reinit_run_t;

// Maintenance asks for the channel at now_ms. With no instance bound: RUN at once. With one bound (sent the REINIT
// input `seq`): WAIT, until RADIO_MUX_REINIT_REPLY_MS later. While another run is asked or running: REFUSE, the run
// unchanged.
radio_mux_reinit_act_t radio_mux_reinit_ask(radio_mux_reinit_run_t *r, uint32_t seq, bool bound, int64_t now_ms);
// The instance's answer to the REINIT input `seq`: accept → RUN, refuse → REFUSE (back to idle). An accept of the last
// run's seq after that run timed out → REBIND, once (the instance let go for nothing). IGNORE for anything else: an
// answer to another seq, a late refusal, an answer while the run runs or after it ended.
radio_mux_reinit_act_t radio_mux_reinit_answer(radio_mux_reinit_run_t *r, uint32_t seq, bool accept);
// Time passes: REFUSE (back to idle) once an asked run reaches its deadline, WAIT before it, IGNORE when nothing is
// asked.
radio_mux_reinit_act_t radio_mux_reinit_tick(radio_mux_reinit_run_t *r, int64_t now_ms);
// The work ended: back to idle.
void radio_mux_reinit_done(radio_mux_reinit_run_t *r);

#ifdef __cplusplus
}
#endif
