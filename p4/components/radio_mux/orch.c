// The channel/mode orchestrator's runtime, on the mux owner task: the registered mode instances, the inputs it delivers
// to them (BIND, UNBIND, LOST, BACK; REINIT once maintenance asks) and their sequence numbers. The binding table is the channels' stored modes (no record of its
// own); the decisions are in orch_core.c.
#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "radio_mux_priv.h"

static const char *TAG = "radio_mux";

static const char *const input_names[] = {
    [RADIO_MUX_IN_NONE] = "none", [RADIO_MUX_IN_BIND] = "bind", [RADIO_MUX_IN_UNBIND] = "unbind",
    [RADIO_MUX_IN_REINIT] = "reinit", [RADIO_MUX_IN_LOST] = "lost", [RADIO_MUX_IN_BACK] = "back",
};

static size_t slot_of(const struct radio_mux_chan *ch)
{
    return (size_t)(ch - g_mux.chans);
}

orch_inst_t *orch_find(radio_mux_mode_t mode)
{
    for (size_t i = 0; i < g_mux.n_insts; i++) {
        if (g_mux.insts[i].inst.mode == mode) {
            return &g_mux.insts[i];
        }
    }
    return NULL;
}

// Owner: one input to `oi` about channel slot `slot`, with the next seq of that instance (of that channel's instance
// for a per_channel one).
static void deliver(orch_inst_t *oi, size_t slot, radio_mux_input_type_t type, radio_mux_chan_t *h,
                    radio_mux_fault_kind_t kind, const char *reason)
{
    const size_t s = oi->inst.per_channel ? slot : 0;
    radio_mux_input_t in = {.type = type, .seq = ++oi->seq[s], .ch = h, .kind = kind};

    snprintf(in.reason, sizeof(in.reason), "%s", reason ? reason : "");
    ESP_LOGI(TAG, "%s: %s %s (seq %" PRIu32 ")%s%s", g_mux.chans[slot].id, radio_mux_mode_name(oi->inst.mode),
             input_names[type], in.seq, in.reason[0] ? ": " : "", in.reason);
    oi->inst.input(&in, oi->inst.ctx);
}

// The orchestrator's own report for the instance on `ch` (an instance reports through radio_mux_report).
static void set_mode_state(struct radio_mux_chan *ch, const char *state, const char *reason)
{
    mux_lock();
    snprintf(ch->mode_state, sizeof(ch->mode_state), "%s", state);
    snprintf(ch->mode_reason, sizeof(ch->mode_reason), "%s", reason ? reason : "");
    mux_unlock();
}

void orch_bind(mux_port_t *p, orch_inst_t *oi, radio_mux_chan_t *h)
{
    struct radio_mux_chan *ch = p->chan;
    const size_t slot = slot_of(ch);
    const radio_mux_input_type_t type = radio_mux_orch_input_for(ch->chan_state, RADIO_MUX_CS_READY, true,
                                                                 oi->held[slot]);

    oi->held[slot] = true;
    if (g_mux.maint_ending && p == &g_mux.c6) {
        // The end of a maintenance: what the instance showed before it is stale; a maintenance waits for its report.
        set_mode_state(ch, "starting", NULL);
    }
    // NONE: the instance already holds the binding on a ready channel (a re-attach of the transport): a new handle is
    // a bind back.
    deliver(oi, slot, type == RADIO_MUX_IN_NONE ? RADIO_MUX_IN_BACK : type, h, RADIO_MUX_FAULT_NONE, NULL);
}

void orch_leave(mux_port_t *p, orch_inst_t *oi, radio_mux_input_type_t type, radio_mux_fault_kind_t kind,
                const char *reason)
{
    struct radio_mux_chan *ch = p->chan;
    const size_t slot = slot_of(ch);

    if (type == RADIO_MUX_IN_UNBIND) {
        // Nothing is bound: the binding goes, and the state with it (a report through the old handle after this is
        // refused once the next instance binds).
        oi->held[slot] = false;
        set_mode_state(ch, "", NULL);
    }
    deliver(oi, slot, type, p->att_handle, kind, reason);
}

void orch_release(struct radio_mux_chan *ch, radio_mux_mode_t mode)
{
    orch_inst_t *oi = orch_find(mode);
    const size_t slot = slot_of(ch);

    if (oi == NULL || !oi->held[slot]) {
        return;
    }
    oi->held[slot] = false;
    deliver(oi, slot, RADIO_MUX_IN_UNBIND, NULL, RADIO_MUX_FAULT_NONE, NULL);
    set_mode_state(ch, "", NULL);
}

void orch_forget_slot(size_t slot)
{
    for (size_t i = 0; i < g_mux.n_insts; i++) {
        g_mux.insts[i].held[slot] = false;
    }
}

void orch_reinit_reply(radio_mux_chan_t *h, uint32_t seq, esp_err_t verdict, const char *reason)
{
    mux_port_t *p;
    uint32_t gen;
    const struct radio_mux_chan *ch = mux_handle_chan(h, &p, &gen);

    if (ch == NULL || p->chan != ch || !radio_mux_handle_current(gen, atomic_load(&p->att_gen))) {
        return; // a stale handle
    }
    const radio_mux_reinit_act_t act = radio_mux_reinit_answer(&g_mux.reinit, seq, verdict == ESP_OK);

    if (act == RADIO_MUX_RA_IGNORE) {
        ESP_LOGD(TAG, "%s: reinit answer %" PRIu32 " ignored (none asked: a notice, or a late answer)", ch->id, seq);
        return;
    }
    if (act == RADIO_MUX_RA_REBIND) {
        // The instance let go after the maintenance gave up on it: nothing will bind it back but this.
        orch_inst_t *oi = orch_find(ch->mode);

        if (oi && p->att.state == RADIO_MUX_AS_ATTACHED && ch->chan_state == RADIO_MUX_CS_READY &&
                !atomic_load(&g_mux.maint_running)) {
            ESP_LOGW(TAG, "%s: %s let go after the maintenance gave up: bound back", ch->id,
                     radio_mux_mode_name(oi->inst.mode));
            orch_bind(p, oi, p->att_handle);
        }
        return;
    }
    mux_maint_answer(act, verdict, reason);
}

radio_mux_reinit_act_t orch_reinit_ask(mux_port_t *p, orch_inst_t *oi, radio_mux_work_t work, int64_t now_ms)
{
    const size_t slot = slot_of(p->chan);
    const uint32_t seq = oi ? oi->seq[oi->inst.per_channel ? slot : 0] + 1 : 0; // what deliver() sends next
    const radio_mux_reinit_act_t act = radio_mux_reinit_ask(&g_mux.reinit, seq, oi != NULL, now_ms);

    if (oi && act == RADIO_MUX_RA_WAIT) {
        deliver(oi, slot, RADIO_MUX_IN_REINIT, p->att_handle, RADIO_MUX_FAULT_NONE, radio_mux_work_reason(work));
    }
    return act;
}

void orch_reinit_notice(mux_port_t *p, radio_mux_work_t work)
{
    orch_inst_t *oi = orch_find(p->chan->mode);

    if (oi && p->att.state != RADIO_MUX_AS_ATTACHED) {
        deliver(oi, slot_of(p->chan), RADIO_MUX_IN_REINIT, p->att_handle, RADIO_MUX_FAULT_NONE,
                radio_mux_work_reason(work));
    }
}

void orch_lost(struct radio_mux_chan *ch, radio_mux_chan_t *h, radio_mux_fault_kind_t kind, const char *reason)
{
    orch_inst_t *oi = orch_find(ch->mode);

    if (oi && radio_mux_orch_input_for(RADIO_MUX_CS_REINIT, RADIO_MUX_CS_FAULT, true, true) == RADIO_MUX_IN_LOST) {
        // Shown at once: an instance with no current handle (the channel faulted before its first bind) cannot report.
        set_mode_state(ch, radio_mux_orch_lost_state(oi->inst.mode, kind), reason);
        deliver(oi, slot_of(ch), RADIO_MUX_IN_LOST, h, kind, reason);
    }
}

void orch_fill_modes(radio_mux_snapshot_t *out)
{
    out->n_modes = 0;
    for (size_t i = 0; i < g_mux.n_insts; i++) {
        const orch_inst_t *oi = &g_mux.insts[i];
        bool listed = false;

        for (size_t c = 0; c < out->n; c++) {
            if (out->chans[c].stored_mode != oi->inst.mode || (listed && !oi->inst.per_channel)) {
                continue;
            }
            out->modes[out->n_modes++] = (radio_mux_mode_info_t) {
                .mode = oi->inst.mode, .chan = (int8_t)c, .reboot_on_change = oi->inst.reboot_on_change,
            };
            listed = true;
        }
        if (!listed && !oi->inst.per_channel) {
            out->modes[out->n_modes++] = (radio_mux_mode_info_t) {
                .mode = oi->inst.mode, .chan = -1, .reboot_on_change = oi->inst.reboot_on_change,
            };
        }
    }
}

/* ---- registration ---- */

esp_err_t radio_mux_register_instance(const radio_mux_instance_t *inst)
{
    ESP_RETURN_ON_FALSE(inst && inst->input, ESP_ERR_INVALID_ARG, TAG, "an instance needs input");
    ESP_RETURN_ON_FALSE(inst->mode == RADIO_MUX_MODE_OTBR || inst->mode == RADIO_MUX_MODE_ZIGGURAT ||
                        inst->mode == RADIO_MUX_MODE_RCP_PROXY, ESP_ERR_INVALID_ARG, TAG, "no instance for mode %d",
                        (int)inst->mode);
    ESP_RETURN_ON_FALSE(!atomic_load(&g_mux.started), ESP_ERR_INVALID_STATE, TAG, "register before radio_mux_start");
    ESP_RETURN_ON_FALSE(!orch_find(inst->mode), ESP_ERR_INVALID_STATE, TAG, "%s is already registered",
                        radio_mux_mode_name(inst->mode));
    ESP_RETURN_ON_FALSE(g_mux.n_insts < RADIO_MUX_RULES_MAX, ESP_ERR_NO_MEM, TAG, "too many instances");
    orch_inst_t *oi = &g_mux.insts[g_mux.n_insts++];

    memset(oi, 0, sizeof(*oi));
    oi->inst = *inst;
    g_mux.rules.rules[g_mux.rules.n++] = (radio_mux_rule_t) {
        .mode = inst->mode, .needs = inst->needs, .limit = inst->limit, .reboot_on_change = inst->reboot_on_change,
    };
    ESP_LOGI(TAG, "%s instance registered", radio_mux_mode_name(inst->mode));
    return ESP_OK;
}

esp_err_t radio_mux_set_builtin(const radio_mux_builtin_t *b)
{
    ESP_RETURN_ON_FALSE(b && b->id && b->present && b->boot_steps && b->push && b->rom_recover, ESP_ERR_INVALID_ARG, TAG,
                        "the built-in RCP needs an id and its ops");
    ESP_RETURN_ON_FALSE(!atomic_load(&g_mux.started), ESP_ERR_INVALID_STATE, TAG, "set it before radio_mux_start");
    g_mux.builtin = b;
    ESP_LOGI(TAG, "built-in RCP: %s (%s)", b->id, b->chip ? b->chip : "?");
    return ESP_OK;
}

const radio_mux_builtin_t *radio_mux_builtin(void)
{
    return g_mux.builtin;
}
