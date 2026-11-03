#pragma once

// Mesh routing (spec docs/superpowers/specs/2026-11-03-mesh-routing-design.md): a custom OMR prefix, and the route
// into the mesh announced by DHCPv6-PD, RIPng or BGP on the backbone. Public interface of the component.

#include "esp_err.h"
#include "sdkconfig.h"
#include "routing_types.h"
#include "settings.h"
#include "announce.h"
#include "bgp_msg.h"
#include "ripng.h"

#ifndef CONFIG_IDF_TARGET_LINUX
#include "openthread/instance.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Load the stored settings (defaults when nothing or nothing valid is stored, or NVS is unavailable) and start the
 * routing task (core MGMT_CORE): it runs the method's sender on the backbone and polls OpenThread every 500 ms.
 * Call it once, after NVS is initialised and before the border router can come up. ESP_ERR_INVALID_STATE when called
 * twice, ESP_ERR_NO_MEM.
 */
esp_err_t routing_start(void);

/**
 * The br_core border-router-up hook (br_core_set_border_router_up_cb): runs with the OpenThread lock held. Starts the
 * prefix publisher (it learns the self-generated ULA first), then applies the latest settings: OMR config, the ULA
 * entry next to a custom prefix, DHCPv6-PD on only with method pd. What fails is retried at the task's next state read.
 */
void routing_on_border_router_up(otInstance *instance);

/**
 * Store validated settings (routing_settings_from_json) in NVS, then hand them to the routing task, which applies them
 * live within about 100 ms. ESP_ERR_INVALID_STATE before routing_start; the NVS error when storing failed (nothing
 * is applied then).
 */
esp_err_t routing_apply(const routing_settings_t *s);

/**
 * The latest stored settings and the status the task last published (it refreshes it after every wake-up, at least
 * every 100 ms). Either pointer may be NULL. ESP_ERR_INVALID_STATE (not logged) before routing_start or when it failed.
 */
esp_err_t routing_get(routing_settings_t *settings, routing_status_t *status);

/**
 * What a PUT needs to validate against: the board's own IPv6 addresses on the backbone and management interfaces, and
 * the prefixes on-link on the backbone (the /64s of the backbone's own addresses and the on-link prefixes of the
 * received RAs, otBorderRoutingGetNextPrefixTableEntry). ESP_ERR_TIMEOUT when the OpenThread lock was not free within
 * 100 ms: `ctx` then holds everything but the RA prefixes.
 */
esp_err_t routing_validate_ctx_fill(routing_validate_ctx_t *ctx);

/**
 * The figures for /metrics, as of the task's last status. ESP_ERR_INVALID_STATE (not logged) before routing_start or
 * when it failed.
 */
esp_err_t routing_metrics_view(routing_metrics_view_t *out);

/** Register the console command `routing` (`routing show`); the console must be initialised. */
esp_err_t routing_console_register(void);

#ifdef __cplusplus
}
#endif
#endif
