// Announcement set (spec section 5): pure and host-tested.
#include "announce.h"

#include <string.h>

#include "routing_util.h"

static bool set_contains(const routing_prefix_set_t *set, const routing_prefix64_t *prefix)
{
    for (uint8_t i = 0; i < set->count; i++) {
        if (routing_prefix64_eq(&set->p[i], prefix)) {
            return true;
        }
    }
    return false;
}

/* Append unless already present; the set never exceeds ROUTING_MAX_ANNOUNCED. */
static void set_add(routing_prefix_set_t *set, const routing_prefix64_t *prefix)
{
    if (set->count < ROUTING_MAX_ANNOUNCED && !set_contains(set, prefix)) {
        set->p[set->count++] = *prefix;
    }
}

void routing_announce_compute(const routing_announce_input_t *in, routing_prefix_set_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!in->br_up || (in->method != ROUTING_METHOD_RIPNG && in->method != ROUTING_METHOD_BGP)) {
        return;
    }
    if (in->omr_mode == ROUTING_OMR_CUSTOM && in->custom_in_netdata) {
        set_add(out, &in->custom);
    }
    if (in->announce_ula && in->ula_known && in->ula_in_netdata) {
        set_add(out, &in->ula);
    }
}

/* Every prefix of `from` that `other` lacks. */
static void set_minus(const routing_prefix_set_t *from, const routing_prefix_set_t *other, routing_prefix_set_t *out)
{
    memset(out, 0, sizeof(*out));
    for (uint8_t i = 0; i < from->count; i++) {
        if (!set_contains(other, &from->p[i])) {
            set_add(out, &from->p[i]);
        }
    }
}

void routing_announce_diff(const routing_prefix_set_t *old_set, const routing_prefix_set_t *new_set,
                           routing_prefix_set_t *added, routing_prefix_set_t *removed)
{
    set_minus(new_set, old_set, added);
    set_minus(old_set, new_set, removed);
}
