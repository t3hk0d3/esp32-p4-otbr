#include <string.h>
#include "recovery_policy.h"

bool rcp_sync_recovery_wanted(const rcp_sync_recovery_input_t *in)
{
    if (!in->jumper_present || in->already_attempted || !in->base_version || in->base_version[0] == '\0') {
        return false;
    }
    if (!in->updater_reachable) {
        return true;
    }
    return in->updater_version == NULL || strcmp(in->updater_version, in->base_version) != 0;
}
