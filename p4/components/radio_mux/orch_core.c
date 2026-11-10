// Decisions of the channel/mode orchestrator. Pure: no ESP-IDF calls.
#include <string.h>

#include "radio_mux_orch_core.h"

static const char *const chan_state_names[] = {
    [RADIO_MUX_CS_ABSENT] = "absent",
    [RADIO_MUX_CS_REINIT] = "reinit",
    [RADIO_MUX_CS_READY] = "ready",
    [RADIO_MUX_CS_FAULT] = "fault",
};

static const char *const work_reasons[] = {
    [RADIO_MUX_WORK_NONE] = "",
    [RADIO_MUX_WORK_BOOT_STEPS] = "boot steps",
    [RADIO_MUX_WORK_DETECT] = "detecting firmware",
    [RADIO_MUX_WORK_PUSH] = "firmware push",
    [RADIO_MUX_WORK_ROM_RECOVERY] = "ROM recovery",
};

const char *radio_mux_chan_state_name(radio_mux_chan_state_t s)
{
    return (s >= RADIO_MUX_CS_ABSENT && s <= RADIO_MUX_CS_FAULT) ? chan_state_names[s] : NULL;
}

const char *radio_mux_work_reason(radio_mux_work_t w)
{
    return (w >= RADIO_MUX_WORK_NONE && w <= RADIO_MUX_WORK_ROM_RECOVERY) ? work_reasons[w] : "";
}

const char *radio_mux_busy_reason(radio_mux_work_t w)
{
    switch (w) {
    case RADIO_MUX_WORK_BOOT_STEPS:
        return "the C6's boot steps run";
    case RADIO_MUX_WORK_PUSH:
    case RADIO_MUX_WORK_ROM_RECOVERY:
        return "a C6 job holds the radio";
    default:
        return "";
    }
}

static bool mode_used_by_other(const radio_mux_chan_view_t *chans, size_t n, size_t target, radio_mux_mode_t mode,
                               size_t *first_index, size_t *count)
{
    *count = 0;
    for (size_t i = 0; i < n; i++) {
        if (i != target && chans[i].mode == mode) {
            if (*count == 0 && first_index != NULL) {
                *first_index = i;
            }
            (*count)++;
        }
    }
    return *count > 0;
}

radio_mux_verdict_t radio_mux_orch_check_bind(const radio_mux_rules_t *rules, const radio_mux_chan_view_t *chans,
                                              size_t n, size_t target, radio_mux_mode_t want, bool in_reinit,
                                              size_t *move_from)
{
    size_t first = 0, count = 0;

    if (want == RADIO_MUX_MODE_UNAVAILABLE || want == RADIO_MUX_MODE_INVALID) {
        return RADIO_MUX_REFUSE_BAD_VALUE;
    }
    if (in_reinit) {
        return RADIO_MUX_REFUSE_BUSY; // maintenance holds the channel: try again once it is ready
    }
    if (target >= n || !chans[target].present || !chans[target].supported) {
        return RADIO_MUX_REFUSE_UNAVAILABLE;
    }
    const radio_mux_rule_t *rule = radio_mux_rule_of(rules, want);

    if (want != RADIO_MUX_MODE_OFF && want != RADIO_MUX_MODE_RCP_PROXY && rule == NULL) {
        return RADIO_MUX_REFUSE_NOT_AVAILABLE;
    }
    if (!radio_mux_fits(rules, want, chans[target].fw)) {
        return RADIO_MUX_REFUSE_INCOMPATIBLE;
    }
    const radio_mux_rule_t *leaving = radio_mux_rule_of(rules, chans[target].mode);

    // otbr on a channel leaving a mode that reboots (Zigbee): the reboot starts OpenThread there, also when Thread runs
    // on another channel (*move_from; n when none), which the reboot's store turns off. A live start or move now would
    // probe a channel whose instance still holds it, and fail.
    if (want == RADIO_MUX_MODE_OTBR && chans[target].mode != want && leaving != NULL && leaving->reboot_on_change) {
        if (move_from != NULL) {
            *move_from = mode_used_by_other(chans, n, target, RADIO_MUX_MODE_OTBR, &first, &count) ? first : n;
        }
        return RADIO_MUX_APPLY_REBOOT;
    }
    if (rule != NULL && rule->limit > 0 && mode_used_by_other(chans, n, target, want, &first, &count) &&
            count >= rule->limit) {
        if (want == RADIO_MUX_MODE_OTBR) {
            if (move_from != NULL) {
                *move_from = first;
            }
            return RADIO_MUX_MOVE;
        }
        return RADIO_MUX_REFUSE_MODE_TAKEN;
    }
    // otbr where no channel has it: a start, run like a move from nowhere (probe, start, store once it runs).
    if (want == RADIO_MUX_MODE_OTBR && chans[target].mode != RADIO_MUX_MODE_OTBR) {
        if (move_from != NULL) {
            *move_from = n;
        }
        return RADIO_MUX_MOVE;
    }
    if (chans[target].mode != want && ((rule != NULL && rule->reboot_on_change) ||
                                       (leaving != NULL && leaving->reboot_on_change))) {
        return RADIO_MUX_APPLY_REBOOT;
    }
    // The otbr channel leaving otbr: a stop, run like a move to nowhere (drain, stop, store once it stopped).
    if (chans[target].mode == RADIO_MUX_MODE_OTBR && want != RADIO_MUX_MODE_OTBR) {
        if (move_from != NULL) {
            *move_from = target;
        }
        return RADIO_MUX_MOVE;
    }
    return RADIO_MUX_APPLY;
}

bool radio_mux_orch_should_bind(radio_mux_chan_state_t cs, bool fits, bool bound)
{
    return cs == RADIO_MUX_CS_READY && fits && !bound;
}

radio_mux_input_type_t radio_mux_orch_input_for(radio_mux_chan_state_t before, radio_mux_chan_state_t after, bool fits,
                                                bool bound)
{
    if (after == RADIO_MUX_CS_READY) {
        if (!fits) {
            return RADIO_MUX_IN_NONE;
        }
        if (!bound) {
            return RADIO_MUX_IN_BIND;
        }
        return before == RADIO_MUX_CS_READY ? RADIO_MUX_IN_NONE : RADIO_MUX_IN_BACK;
    }
    if ((after == RADIO_MUX_CS_ABSENT || after == RADIO_MUX_CS_FAULT) && bound &&
            (before == RADIO_MUX_CS_READY || before == RADIO_MUX_CS_REINIT)) {
        return RADIO_MUX_IN_LOST;
    }
    return RADIO_MUX_IN_NONE;
}

size_t radio_mux_singleton_dedupe(radio_mux_mode_t *modes, bool *changed, size_t n)
{
    bool seen_otbr = false, seen_zig = false;
    size_t count = 0;

    for (size_t i = 0; i < n; i++) {
        bool *seen = modes[i] == RADIO_MUX_MODE_OTBR ? &seen_otbr :
                     modes[i] == RADIO_MUX_MODE_ZIGGURAT ? &seen_zig : NULL;

        changed[i] = false;
        if (seen == NULL) {
            continue;
        }
        if (*seen) {
            modes[i] = RADIO_MUX_MODE_OFF;
            changed[i] = true;
            count++;
        }
        *seen = true;
    }
    return count;
}

const char *radio_mux_orch_lost_state(radio_mux_mode_t mode, radio_mux_fault_kind_t kind)
{
    return mode == RADIO_MUX_MODE_OTBR && kind == RADIO_MUX_FAULT_FIRMWARE ? "fault" : "no_radio";
}

static bool state_in(const char *state, const char *const *list, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (strcmp(state, list[i]) == 0) {
            return true;
        }
    }
    return false;
}

radio_mux_settle_t radio_mux_orch_settle(radio_mux_chan_state_t cs, bool instance, const char *mode_state,
                                         const char *mode_reason)
{
    static const char *const wait[] = {"", "starting", "releasing"};
    static const char *const up[] = {"running", "sniffer", "listening", "connecting", "backoff", "connected"};
    const char *state = mode_state ? mode_state : "";

    if (!instance) {
        return RADIO_MUX_SETTLE_NONE;
    }
    if (cs == RADIO_MUX_CS_REINIT) {
        return RADIO_MUX_SETTLE_WAIT;
    }
    if (cs != RADIO_MUX_CS_READY) {
        return RADIO_MUX_SETTLE_DOWN;
    }
    if (state_in(state, wait, sizeof(wait) / sizeof(wait[0]))) {
        return RADIO_MUX_SETTLE_WAIT;
    }
    if (strcmp(state, "fault") == 0 && (mode_reason == NULL || mode_reason[0] == '\0')) {
        return RADIO_MUX_SETTLE_WAIT; // Thread's reason follows its state
    }
    return state_in(state, up, sizeof(up) / sizeof(up[0])) ? RADIO_MUX_SETTLE_UP : RADIO_MUX_SETTLE_DOWN;
}

radio_mux_reinit_act_t radio_mux_reinit_ask(radio_mux_reinit_run_t *r, uint32_t seq, bool bound, int64_t now_ms)
{
    if (r->phase != RADIO_MUX_RP_IDLE) {
        return RADIO_MUX_RA_REFUSE;
    }
    r->seq = seq;
    if (!bound) {
        r->phase = RADIO_MUX_RP_RUNNING;
        return RADIO_MUX_RA_RUN;
    }
    r->phase = RADIO_MUX_RP_ASKED;
    r->deadline_ms = now_ms + RADIO_MUX_REINIT_REPLY_MS;
    return RADIO_MUX_RA_WAIT;
}

radio_mux_reinit_act_t radio_mux_reinit_answer(radio_mux_reinit_run_t *r, uint32_t seq, bool accept)
{
    if (r->late_pending && seq == r->late_seq && r->phase != RADIO_MUX_RP_RUNNING) {
        r->late_pending = false;
        if (!accept) {
            return RADIO_MUX_RA_IGNORE;
        }
        if (r->phase == RADIO_MUX_RP_ASKED) {
            r->phase = RADIO_MUX_RP_RUNNING; // the instance let go for the earlier run: the channel is free now
            return RADIO_MUX_RA_RUN;
        }
        return RADIO_MUX_RA_REBIND;
    }
    if (r->phase != RADIO_MUX_RP_ASKED || seq != r->seq) {
        return RADIO_MUX_RA_IGNORE;
    }
    r->phase = accept ? RADIO_MUX_RP_RUNNING : RADIO_MUX_RP_IDLE;
    return accept ? RADIO_MUX_RA_RUN : RADIO_MUX_RA_REFUSE;
}

radio_mux_reinit_act_t radio_mux_reinit_tick(radio_mux_reinit_run_t *r, int64_t now_ms)
{
    if (r->phase != RADIO_MUX_RP_ASKED) {
        return RADIO_MUX_RA_IGNORE;
    }
    if (now_ms < r->deadline_ms) {
        return RADIO_MUX_RA_WAIT;
    }
    r->phase = RADIO_MUX_RP_IDLE;
    r->late_pending = true;
    r->late_seq = r->seq;
    return RADIO_MUX_RA_REFUSE;
}

void radio_mux_reinit_done(radio_mux_reinit_run_t *r)
{
    r->phase = RADIO_MUX_RP_IDLE;
    r->late_pending = false; // the run's end bound the instance again
}
