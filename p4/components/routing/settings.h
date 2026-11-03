#pragma once

// Routing settings (spec section 6): defaults, the JSON form of GET and PUT /device/routing with its validation, the
// change set between two settings (pure, host-tested), and the NVS copy (target only).

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "sdkconfig.h"
#include "routing_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ROUTING_DEFAULT_HOLD_TIME 90
/** A private 4-byte ASN (RFC 6996), the value of the spec's example; valid, so that the defaults pass validation. */
#define ROUTING_DEFAULT_LOCAL_ASN 4200000001u

#define ROUTING_CHANGE_METHOD (1u << 0)       /**< the active method differs */
#define ROUTING_CHANGE_PREFIXES (1u << 1)     /**< omr or announce_ula differ: re-announce, no session reset */
#define ROUTING_CHANGE_BGP_SESSIONS (1u << 2) /**< peers, ASNs, router ID or hold time differ */

/** Method pd, omr auto, announce_ula false, ROUTING_DEFAULT_LOCAL_ASN, router ID unset, hold time 90, no peers. */
void routing_settings_defaults(routing_settings_t *out);

/**
 * Parse and check a PUT /device/routing body. `status` in the body is ignored. Without "bgp" the BGP settings are
 * taken from `current` (the defaults when NULL); "method", "omr" and "announce_ula" are required. `ctx` (may be NULL:
 * those checks are skipped) gives the on-link prefixes (backbone, and mgmt in split mode), which a custom prefix must
 * not equal, and the board's own addresses, which a peer must not have. Peers taken from `current` get the same address
 * checks as peers in the body.
 *
 * ESP_OK fills `out` (which may be `current`). Otherwise ESP_ERR_INVALID_ARG with `*bad_field` (may be NULL) naming
 * the first offending field, a static string: "body", "method", "omr", "omr.mode", "omr.prefix", "announce_ula",
 * "bgp", "bgp.local_asn", "bgp.router_id", "bgp.hold_time", "bgp.peers"; `out` is left unchanged.
 */
esp_err_t routing_settings_from_json(const char *json, const routing_settings_t *current,
                                     const routing_validate_ctx_t *ctx, routing_settings_t *out, const char **bad_field);

/**
 * The GET /device/routing shape, with "status": null when `status` is NULL. Each peer of `s` gets the status entry
 * with its address; a peer `status` has no entry for (the status predates the settings) is shown idle, uptime 0, no
 * last error, nothing dropped. Malloc'd, the caller frees; NULL on OOM.
 */
char *routing_settings_to_json(const routing_settings_t *s, const routing_status_t *status);

/**
 * The settings from their stored JSON text, checked as a PUT body with the defaults as `current` and no context.
 * NULL text (nothing stored): the defaults, true. Text that does not parse or validate: the defaults, false, with
 * `*bad_field` (may be NULL) naming the offending field. Valid text: those settings, true.
 */
bool routing_settings_from_stored(const char *text, routing_settings_t *out, const char **bad_field);

/** The entry of `st` for the peer `addr`, NULL when `st` has none (it was built from settings without that peer). */
const routing_bgp_peer_status_t *routing_status_peer(const routing_status_t *st, const uint8_t addr[16]);

/** ROUTING_CHANGE_* flags for what differs between `a` and `b`; 0 when they are the same. */
uint32_t routing_settings_diff(const routing_settings_t *a, const routing_settings_t *b);

#ifndef CONFIG_IDF_TARGET_LINUX
/** The stored settings (NVS namespace "routing", key "settings"). Nothing stored or unparsable text: the defaults. */
esp_err_t routing_settings_load(routing_settings_t *out);

/** Store the settings as JSON text in NVS. */
esp_err_t routing_settings_store(const routing_settings_t *s);
#endif

#ifdef __cplusplus
}
#endif
