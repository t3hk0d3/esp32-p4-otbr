#pragma once

// Validation, defaults and change detection of the network names. Pure; host-tested in test_host/.

#include <stdbool.h>
#include <stdint.h>
#include "netnames.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Which apply steps a save needs. */
typedef struct {
    bool hostname;    /**< mdns_hostname_set */
    bool ha_name;     /**< mDNS instance name and the meshcop service */
    bool http_advert; /**< add, remove or rename the _http._tcp service */
    bool dhcp_name;   /**< DHCP client name and a lease renewal */
} netnames_steps_t;

/**
 * Are the settings valid? On false, *field is "hostname", "ha_name" or "dhcp_name" and *reason a short English reason.
 * Hostname and DHCP name: empty, or 1-63 (DHCP name: 1-32) of a-z A-Z 0-9 '-', not starting or ending with '-'. HA name (only when it does
 * not follow the hostname): 1-63 bytes of UTF-8, no control characters, no leading or trailing space.
 */
bool netnames_rules_check(const netnames_settings_t *in, const char **field, const char **reason);

/** Hostname and DHCP name in lower case; the HA name is left as it is. */
void netnames_rules_normalise(netnames_settings_t *s);

/** "esp-otbr-" and the last three MAC bytes in lowercase hex. */
void netnames_rules_default_hostname(const uint8_t mac[6], char out[NETNAMES_NAME_LEN]);

/** The names in effect: defaults resolved, the HA name following the hostname when asked. */
void netnames_rules_effective(const netnames_settings_t *s, const char *default_hostname, char hostname[NETNAMES_NAME_LEN],
                              char ha_name[NETNAMES_NAME_LEN], char dhcp_name[NETNAMES_NAME_LEN]);

/**
 * Steps needed to go from `old` to `new`. The hostname step also runs when nothing else changes and the mDNS library
 * uses another name (`live_hostname`: a conflict rename, or "" when mDNS does not run), so saving again retries the name.
 */
netnames_steps_t netnames_rules_steps(const netnames_settings_t *old, const netnames_settings_t *new,
                                      const char *default_hostname, const char *live_hostname);

#ifdef __cplusplus
}
#endif
