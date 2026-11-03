#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BR_CORE_LEADER_OK,
    BR_CORE_LEADER_NOT_RUNNING,
    BR_CORE_LEADER_NOT_ATTACHED,
    BR_CORE_LEADER_ALREADY,
    BR_CORE_LEADER_NOT_ELIGIBLE,
    BR_CORE_LEADER_WEIGHT_NOT_HIGHER,
} br_core_leader_refusal_t;

#define BR_CORE_LEADER_ADJ_MIN (-16)
#define BR_CORE_LEADER_ADJ_MAX 16

/** True when @p value is within BR_CORE_LEADER_ADJ_MIN..BR_CORE_LEADER_ADJ_MAX. */
bool br_core_leader_adjustment_valid(int value);

/**
 * Whether this board may take over leadership now. Returns the first refusal that applies, checked in order:
 * not running, not attached, already leader, not router eligible, own weight not strictly above the leader's.
 */
br_core_leader_refusal_t br_core_leader_check(bool running, bool attached, bool is_leader, bool router_eligible,
                                              uint8_t own_weight, uint8_t leader_weight);

#ifdef __cplusplus
}
#endif
