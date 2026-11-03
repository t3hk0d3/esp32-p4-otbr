#include "br_core_trel.h"

#include <stdio.h>
#include <string.h>

static void hex8(char out[17], const uint8_t in[8])
{
    for (size_t i = 0; i < 8; i++) {
        snprintf(&out[2 * i], 3, "%02x", in[i]);
    }
}

void br_core_trel_sort_init(br_core_trel_sort_t *s, const uint8_t *own_ext_pan_id)
{
    memset(s, 0, sizeof(*s));
    if (own_ext_pan_id) {
        s->has_own = true;
        memcpy(s->own_ext_pan_id, own_ext_pan_id, sizeof(s->own_ext_pan_id));
    }
}

void br_core_trel_sort_add(br_core_trel_sort_t *s, const br_core_trel_raw_peer_t *peer)
{
    const bool same = s->has_own && memcmp(peer->ext_pan_id, s->own_ext_pan_id, sizeof(s->own_ext_pan_id)) == 0;
    br_core_trel_peer_t *list = same ? s->same_list : s->other_list;
    size_t *n = same ? &s->n_same : &s->n_other;

    if (same) {
        s->same++;
    } else {
        s->other++;
    }
    if (*n >= BR_CORE_TREL_PEERS_MAX) {
        return;
    }
    br_core_trel_peer_t *entry = &list[(*n)++];

    memset(entry, 0, sizeof(*entry));
    hex8(entry->ext_address, peer->ext_address);
    hex8(entry->ext_pan_id, peer->ext_pan_id);
    snprintf(entry->sock_address, sizeof(entry->sock_address), "%s", peer->sock_address ? peer->sock_address : "");
    entry->same_network = same;
    entry->neighbor = peer->neighbor;
    entry->pref_154 = peer->neighbor ? peer->pref_154 : BR_CORE_TREL_PREF_NONE;
    entry->pref_trel = peer->neighbor ? peer->pref_trel : BR_CORE_TREL_PREF_NONE;
}

void br_core_trel_sort_finish(const br_core_trel_sort_t *s, br_core_trel_t *out)
{
    out->peers = s->same;
    out->peers_other = s->other;
    out->peer_count = 0;
    for (size_t i = 0; i < s->n_same && out->peer_count < BR_CORE_TREL_PEERS_MAX; i++) {
        out->peer_list[out->peer_count++] = s->same_list[i];
    }
    for (size_t i = 0; i < s->n_other && out->peer_count < BR_CORE_TREL_PEERS_MAX; i++) {
        out->peer_list[out->peer_count++] = s->other_list[i];
    }
}
