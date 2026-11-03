#include "br_core_leader.h"

bool br_core_leader_adjustment_valid(int value)
{
    return value >= BR_CORE_LEADER_ADJ_MIN && value <= BR_CORE_LEADER_ADJ_MAX;
}

br_core_leader_refusal_t br_core_leader_check(bool running, bool attached, bool is_leader, bool router_eligible,
                                              uint8_t own_weight, uint8_t leader_weight)
{
    if (!running) {
        return BR_CORE_LEADER_NOT_RUNNING;
    }
    if (!attached) {
        return BR_CORE_LEADER_NOT_ATTACHED;
    }
    if (is_leader) {
        return BR_CORE_LEADER_ALREADY;
    }
    if (!router_eligible) {
        return BR_CORE_LEADER_NOT_ELIGIBLE;
    }
    if (own_weight <= leader_weight) {
        return BR_CORE_LEADER_WEIGHT_NOT_HIGHER;
    }
    return BR_CORE_LEADER_OK;
}
