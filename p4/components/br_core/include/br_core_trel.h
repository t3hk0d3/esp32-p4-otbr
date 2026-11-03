#pragma once

// TREL state as br_core reports it, and the pure peer sorter behind it (trel_peers.c). No ESP-IDF or OpenThread
// types, so the sorter is host-tested on its own.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BR_CORE_TREL_PEERS_MAX 8
#define BR_CORE_TREL_PREF_NONE (-1) /**< the neighbour does not use that radio */

typedef struct {
    char ext_address[17];  /**< 16 hex digits */
    char ext_pan_id[17];   /**< 16 hex digits: the Thread network the peer advertises */
    char sock_address[48]; /**< "[ipv6]:port" */
    bool same_network;     /**< the board's own extended PAN ID; false when the board has none */
    bool neighbor;         /**< a Thread neighbour of the board: pref_154 and pref_trel are valid */
    int16_t pref_154;      /**< OpenThread's 15.4 preference 0..255, BR_CORE_TREL_PREF_NONE when not used */
    int16_t pref_trel;     /**< OpenThread's TREL preference 0..255, BR_CORE_TREL_PREF_NONE when not used */
} br_core_trel_peer_t;

/** What OpenThread reports of TREL: small, so the metrics collector keeps one without the peer list. */
typedef struct {
    bool state_valid; /**< the fields below were read from OpenThread; false when it is not up or its lock was busy */
    bool active;      /**< OpenThread runs TREL: enabled and Thread up (otTrelIsEnabled) */
    uint16_t peers;       /**< peers of the board's Thread network */
    uint16_t peers_other; /**< peers advertising another Thread network */
    uint64_t tx_packets, tx_bytes, tx_failures, rx_packets, rx_bytes;
} br_core_trel_state_t;

typedef struct {
    size_t count; /**< entries in peers, at most BR_CORE_TREL_PEERS_MAX: this network's peers first */
    br_core_trel_peer_t peers[BR_CORE_TREL_PEERS_MAX];
} br_core_trel_list_t;

typedef struct {
    bool built;   /**< the firmware has TREL (CONFIG_OPENTHREAD_RADIO_TREL) */
    bool enabled; /**< the setting (br_core_set_trel) */
    br_core_trel_state_t state;
    br_core_trel_list_t list;
} br_core_trel_t;

/** OpenThread's view of one peer as a Thread neighbour (otMultiRadioGetNeighborInfo). */
typedef struct {
    bool found; /**< the lookup succeeded: the peer is a Thread neighbour of the board */
    bool has_154, has_trel;
    uint8_t pref_154, pref_trel;
} br_core_trel_radios_t;

/** One peer of OpenThread's TREL peer table, as the sorter takes it. */
typedef struct {
    uint8_t ext_address[8];
    uint8_t ext_pan_id[8];
    const char *sock_address;
    bool neighbor;
    int16_t pref_154;
    int16_t pref_trel;
} br_core_trel_raw_peer_t;

/** Collects the whole peer table: counts every peer, keeps the first BR_CORE_TREL_PEERS_MAX of each network kind. */
typedef struct {
    bool count_only; /**< count the peers, keep no list */
    bool has_own;
    uint8_t own_ext_pan_id[8];
    uint16_t same, other;
    size_t n_same, n_other;
    br_core_trel_peer_t same_list[BR_CORE_TREL_PEERS_MAX];
    br_core_trel_peer_t other_list[BR_CORE_TREL_PEERS_MAX];
} br_core_trel_sort_t;

/**
 * Start a sort. `own_ext_pan_id`: the board's extended PAN ID, used only when `commissioned` (an active dataset);
 * otherwise the board has no network and every peer is another network's. With `count_only`, peers are counted but
 * not listed.
 */
void br_core_trel_sort_init(br_core_trel_sort_t *s, const uint8_t *own_ext_pan_id, bool commissioned, bool count_only);

/** A peer advertising `ext_pan_id` would be listed: worth reading its address and radios before adding it. */
bool br_core_trel_sort_keeps(const br_core_trel_sort_t *s, const uint8_t ext_pan_id[8]);

/** Set `peer`'s neighbour flag and preferences from a neighbour lookup; a radio the neighbour does not use is NONE. */
void br_core_trel_raw_set_radios(br_core_trel_raw_peer_t *peer, const br_core_trel_radios_t *radios);

/** Count `peer`, and list it when there is room. Only a peer of this network is reported as a neighbour. */
void br_core_trel_sort_add(br_core_trel_sort_t *s, const br_core_trel_raw_peer_t *peer);

/** Write state's peers and peers_other and, unless `list` is NULL, the list: this network's peers first. */
void br_core_trel_sort_finish(const br_core_trel_sort_t *s, br_core_trel_state_t *state, br_core_trel_list_t *list);

#ifdef __cplusplus
}
#endif
