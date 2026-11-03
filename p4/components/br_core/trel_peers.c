#include "br_core_trel.h"

#include <stdio.h>
#include <string.h>

static void hex8(char out[17], const uint8_t in[8])
{
    for (size_t i = 0; i < 8; i++) {
        snprintf(&out[2 * i], 3, "%02x", in[i]);
    }
}

void br_core_trel_sort_init(br_core_trel_sort_t *s, const uint8_t *own_ext_pan_id, bool commissioned, bool count_only)
{
    memset(s, 0, sizeof(*s));
    s->count_only = count_only;
    if (commissioned && own_ext_pan_id) {
        s->has_own = true;
        memcpy(s->own_ext_pan_id, own_ext_pan_id, sizeof(s->own_ext_pan_id));
    }
}

static bool same_network(const br_core_trel_sort_t *s, const uint8_t ext_pan_id[8])
{
    return s->has_own && memcmp(ext_pan_id, s->own_ext_pan_id, sizeof(s->own_ext_pan_id)) == 0;
}

bool br_core_trel_sort_keeps(const br_core_trel_sort_t *s, const uint8_t ext_pan_id[8])
{
    return !s->count_only && (same_network(s, ext_pan_id) ? s->n_same : s->n_other) < BR_CORE_TREL_PEERS_MAX;
}

void br_core_trel_raw_set_radios(br_core_trel_raw_peer_t *peer, const br_core_trel_radios_t *radios)
{
    peer->neighbor = radios->found;
    peer->pref_154 = radios->found && radios->has_154 ? radios->pref_154 : BR_CORE_TREL_PREF_NONE;
    peer->pref_trel = radios->found && radios->has_trel ? radios->pref_trel : BR_CORE_TREL_PREF_NONE;
}

void br_core_trel_sort_add(br_core_trel_sort_t *s, const br_core_trel_raw_peer_t *peer)
{
    const bool same = same_network(s, peer->ext_pan_id);
    // Another network's node is never a neighbour (MLE security): a match is an extended address collision.
    const bool neighbor = same && peer->neighbor;
    br_core_trel_peer_t *list = same ? s->same_list : s->other_list;
    size_t *n = same ? &s->n_same : &s->n_other;

    if (same) {
        s->same++;
    } else {
        s->other++;
    }
    if (!br_core_trel_sort_keeps(s, peer->ext_pan_id)) {
        return;
    }
    br_core_trel_peer_t *entry = &list[(*n)++];

    memset(entry, 0, sizeof(*entry));
    hex8(entry->ext_address, peer->ext_address);
    hex8(entry->ext_pan_id, peer->ext_pan_id);
    snprintf(entry->sock_address, sizeof(entry->sock_address), "%s", peer->sock_address ? peer->sock_address : "");
    entry->same_network = same;
    entry->neighbor = neighbor;
    entry->pref_154 = neighbor ? peer->pref_154 : BR_CORE_TREL_PREF_NONE;
    entry->pref_trel = neighbor ? peer->pref_trel : BR_CORE_TREL_PREF_NONE;
}

void br_core_trel_sort_finish(const br_core_trel_sort_t *s, br_core_trel_state_t *state, br_core_trel_list_t *list)
{
    state->peers = s->same;
    state->peers_other = s->other;
    if (!list) {
        return;
    }
    list->count = 0;
    for (size_t i = 0; i < s->n_same && list->count < BR_CORE_TREL_PEERS_MAX; i++) {
        list->peers[list->count++] = s->same_list[i];
    }
    for (size_t i = 0; i < s->n_other && list->count < BR_CORE_TREL_PEERS_MAX; i++) {
        list->peers[list->count++] = s->other_list[i];
    }
}
