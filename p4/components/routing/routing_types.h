#pragma once

// Plain types shared by the routing modules (spec docs/superpowers/specs/2026-11-03-mesh-routing-design.md): settings,
// prefix sets, status and the metrics view. No OpenThread, lwIP or FreeRTOS types, so that the pure modules are
// host-tested.

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ROUTING_MAX_PEERS 2
#define ROUTING_MAX_ANNOUNCED 2
#define ROUTING_MAX_ONLINK 8     /**< on-link prefixes (backbone, mgmt) a validation context holds */
#define ROUTING_MAX_OWN_ADDRS 12 /**< board addresses a validation context holds */
#define ROUTING_LAST_ERROR_LEN 48

typedef enum {
    ROUTING_METHOD_PD,
    ROUTING_METHOD_RIPNG,
    ROUTING_METHOD_BGP,
    ROUTING_METHOD_OFF,
} routing_method_t;

typedef enum {
    ROUTING_OMR_AUTO,
    ROUTING_OMR_CUSTOM,
} routing_omr_mode_t;

/** A /64 by its first 8 bytes. */
typedef struct {
    uint8_t b[8];
} routing_prefix64_t;

typedef struct {
    routing_prefix64_t p[ROUTING_MAX_ANNOUNCED];
    uint8_t count;
} routing_prefix_set_t;

/** A configured BGP peer. Link-local when `addr` is in fe80::/10; then it is scoped to the backbone interface. */
typedef struct {
    uint8_t addr[16];
    uint32_t asn;
} routing_bgp_peer_t;

typedef struct {
    routing_method_t method;
    routing_omr_mode_t omr_mode;
    routing_prefix64_t omr_prefix; /**< used with ROUTING_OMR_CUSTOM, all zero with ROUTING_OMR_AUTO */
    bool announce_ula;
    /* BGP settings, kept while another method is active */
    uint32_t local_asn;
    bool router_id_set; /**< false: the router ID comes from the backbone IPv4 address */
    uint32_t router_id; /**< host order IPv4, 0 when router_id_set is false */
    uint16_t hold_time; /**< 0 or 3..65535 */
    uint8_t peer_count;
    routing_bgp_peer_t peers[ROUTING_MAX_PEERS];
} routing_settings_t;

/** What a settings check needs to know about the backbone. */
typedef struct {
    routing_prefix64_t onlink[ROUTING_MAX_ONLINK]; /**< prefixes on-link on the backbone or the mgmt netif */
    uint8_t onlink_count;
    uint8_t own_addrs[ROUTING_MAX_OWN_ADDRS][16]; /**< the board's own IPv6 addresses */
    uint8_t own_count;
} routing_validate_ctx_t;

typedef enum {
    ROUTING_BGP_IDLE,
    ROUTING_BGP_CONNECT,
    ROUTING_BGP_ACTIVE,
    ROUTING_BGP_OPENSENT,
    ROUTING_BGP_OPENCONFIRM,
    ROUTING_BGP_ESTABLISHED,
} routing_bgp_state_t;

typedef struct {
    uint8_t addr[16]; /**< the peer this entry is for */
    routing_bgp_state_t state;
    uint32_t uptime_s;
    char last_error[ROUTING_LAST_ERROR_LEN]; /**< "" when none */
    uint32_t received_dropped;
    uint32_t resets;
} routing_bgp_peer_status_t;

typedef struct {
    int64_t last_update_ms_ago; /**< -1 never */
    uint32_t updates_sent;
    uint32_t requests_answered;
    uint32_t responses_dropped;
} routing_ripng_status_t;

typedef struct {
    bool omr_prefix_known;
    routing_prefix64_t omr_prefix; /**< local OMR prefix in use: custom, delegated or ULA */
    bool ula_prefix_known;
    routing_prefix64_t ula_prefix; /**< the self-generated ULA */
    routing_prefix_set_t announced;
    uint32_t bgp_router_id;  /**< host order IPv4 in use, 0 none */
    const char *bgp_blocked; /**< why sessions do not run, NULL when not blocked */
    /**
     * One entry per peer of the settings the status was built from (peers[i] for its peers[i]); they can be older than
     * the stored settings for a moment after a PUT, so consumers pair entries with peers by address
     * (routing_status_peer).
     */
    uint8_t peer_count;
    routing_bgp_peer_status_t peers[ROUTING_MAX_PEERS];
    routing_ripng_status_t ripng;
} routing_status_t;

typedef struct {
    uint8_t addr[16];
    routing_bgp_state_t state;
    uint32_t uptime_s;
    uint32_t received_dropped;
    uint32_t resets;
} routing_metrics_peer_t;

/** Figures the metrics renderer reads. */
typedef struct {
    routing_method_t method;
    uint8_t announced_count;
    bool omr_custom;
    uint8_t peer_count;
    routing_metrics_peer_t peers[ROUTING_MAX_PEERS];
    uint32_t ripng_updates_sent;
    uint32_t ripng_requests_answered;
    uint32_t ripng_responses_dropped;
} routing_metrics_view_t;

#ifdef __cplusplus
}
#endif
