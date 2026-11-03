#pragma once

// Prefix publisher (spec section 5): puts the OMR settings onto the Thread mesh (OpenThread's OMR config, the extra
// ULA on-mesh entry next to a custom prefix, the DHCPv6-PD client) through the routing_ot_ops_t seam, so that its
// logic is host-tested. ot_ops.c is the OpenThread implementation of the seam (target only).
//
// OpenThread lock: every function here, and every op, runs with the OpenThread lock held by the caller. Neither the
// publisher nor the ops take it themselves.
//
// Order at border-router up: routing_publisher_init runs before the DHCPv6-PD client is enabled and before any custom
// OMR config is set (that is, before the first routing_publisher_apply), while OpenThread's OMR config is still auto,
// so that the prefix it learns is the self-generated ULA (spec section 5). In ESP-IDF 6.1 otBorderRoutingGetOmrPrefix
// returns the generated prefix in every config (routing_manager.cpp, RoutingManager::GetOmrPrefix), but the order is
// the contract, not that detail.

#include <stdbool.h>
#include <stdint.h>

#include "routing_types.h"
#include "announce.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Route preferences the ops take: low and medium (OT_ROUTE_PREFERENCE_LOW / _MED). */
#define ROUTING_PREF_LOW ((int8_t) - 1)
#define ROUTING_PREF_MED ((int8_t)0)

/** The OpenThread calls the publisher makes. Results are otError-like: 0 is success. */
typedef struct {
    /** otBorderRoutingSetOmrConfig; `prefix` is used with ROUTING_OMR_CUSTOM and NULL with ROUTING_OMR_AUTO. */
    int (*set_omr_config)(routing_omr_mode_t mode, const routing_prefix64_t *prefix, int8_t pref);
    /** otBorderRoutingGetOmrPrefix: the self-generated ULA OMR prefix. */
    int (*get_omr_prefix)(routing_prefix64_t *out);
    /** otBorderRouterAddOnMeshPrefix (preferred, SLAAC, on-mesh, stable); replaces a local entry for the prefix. */
    int (*add_on_mesh)(const routing_prefix64_t *prefix, int8_t pref, bool default_route);
    /** otBorderRouterRemoveOnMeshPrefix. */
    int (*remove_on_mesh)(const routing_prefix64_t *prefix);
    /** otBorderRouterRegister: send the local Network Data changes to the leader. */
    int (*register_netdata)(void);
    /** otBorderRoutingDhcp6PdSetEnabled. */
    int (*pd_set_enabled)(bool enabled);
    /** The /64 is an on-mesh prefix in the leader Network Data. */
    bool (*netdata_has_prefix)(const routing_prefix64_t *prefix);
    /** This border router publishes ::/0 as an external route in Network Data. */
    bool (*default_route_published)(void);
} routing_ot_ops_t;

/** What the publisher learned and what it applied, so that it only issues the changes. Callers do not touch it. */
typedef struct {
    const routing_ot_ops_t *ops;
    bool ula_known;
    routing_prefix64_t ula; /**< the self-generated ULA, when ula_known */
    bool have_settings;     /**< apply ran since init */
    routing_omr_mode_t want_omr_mode;
    routing_prefix64_t want_custom;
    bool want_announce_ula;
    bool omr_applied; /**< omr_mode / omr_prefix are what OpenThread has */
    routing_omr_mode_t omr_mode;
    routing_prefix64_t omr_prefix;
    bool ula_published; /**< the ULA on-mesh entry is ours, with ula_default_route */
    bool ula_default_route;
    bool ula_failed; /**< the last attempt to add or remove the ULA entry failed (logged once) */
    bool omr_failed; /**< the last set_omr_config failed (logged once) */
    bool pd_applied; /**< pd_enabled is what OpenThread has */
    bool pd_enabled;
    bool pd_failed; /**< the last pd_set_enabled failed (logged once) */
} routing_publisher_t;

/** The ops on OpenThread (ot_ops.c, target only). They use esp_openthread_get_instance(). */
extern const routing_ot_ops_t routing_ot_ops_openthread;

/**
 * Start a publisher at border-router up (see the order above): forgets what an earlier one applied and records the
 * self-generated ULA through get_omr_prefix. If that fails the ULA stays unknown and
 * routing_publisher_sync_default_route retries. Makes no other call.
 */
void routing_publisher_init(routing_publisher_t *p, const routing_ot_ops_t *ops);

/**
 * Put `s` onto the mesh, issuing only what differs from what was applied before:
 * - custom: set_omr_config(CUSTOM, prefix, medium); with announce_ula and the ULA known (and not equal to the custom
 *   prefix) the ULA is added on-mesh with preference low and the default-route flag of default_route_published, then
 *   register_netdata; otherwise the ULA entry is removed (and registered) if the publisher added it;
 * - auto: set_omr_config(AUTO, NULL, medium) and removes the ULA entry if the publisher added it;
 * - pd_set_enabled(method == pd).
 * A failed op is not recorded as applied and is issued again by the next apply (its failure is logged once until it
 * succeeds). Nothing else on the mesh is touched.
 *
 * Returns true when everything is at its wanted state afterwards: the OMR config, the DHCPv6-PD client and the ULA
 * entry (published exactly when wanted; the ULA not yet known counts as nothing wanted). false: an op failed, call
 * again to retry.
 */
bool routing_publisher_apply(routing_publisher_t *p, const routing_settings_t *s);

/**
 * Periodic check: learns the ULA if init could not, then brings the ULA entry in line with the applied settings. When
 * the entry is published and default_route_published changed, it is added again with the new flag and registered.
 * Nothing changes on the mesh while nothing changed in OpenThread.
 */
void routing_publisher_sync_default_route(routing_publisher_t *p);

/**
 * Fill `ula`, `ula_known`, `custom_in_netdata` (the applied settings are custom and their prefix is in Network Data)
 * and `ula_in_netdata` (ULA known and in Network Data) of `io`; the other fields are left alone.
 */
void routing_publisher_prefix_state(const routing_publisher_t *p, routing_announce_input_t *io);

#ifdef __cplusplus
}
#endif
