#pragma once

// Announcement set (spec section 5): the protocol-neutral set of prefixes RIPng and BGP announce, and its diff against
// what was announced last. Pure, host-tested.

#include <stdbool.h>

#include "routing_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** What the announcement set depends on; the publisher fills it. */
typedef struct {
    routing_method_t method;
    routing_omr_mode_t omr_mode;
    routing_prefix64_t custom; /**< the custom OMR prefix, used with ROUTING_OMR_CUSTOM */
    bool announce_ula;
    bool ula_known;
    routing_prefix64_t ula; /**< the self-generated ULA, used when ula_known */
    bool custom_in_netdata;
    bool ula_in_netdata;
    bool br_up; /**< the border router runs and Thread is attached */
} routing_announce_input_t;

/**
 * The prefixes to announce: with method ripng or bgp and the border router up, the custom prefix (custom mode, in
 * Network Data) first, then the ULA (box ticked, known, in Network Data). A ULA equal to the custom prefix is listed
 * once. Empty otherwise.
 */
void routing_announce_compute(const routing_announce_input_t *in, routing_prefix_set_t *out);

/** Set difference, order-independent: `added` is in `new_set` but not `old_set`, `removed` the other way round. */
void routing_announce_diff(const routing_prefix_set_t *old_set, const routing_prefix_set_t *new_set,
                           routing_prefix_set_t *added, routing_prefix_set_t *removed);

#ifdef __cplusplus
}
#endif
