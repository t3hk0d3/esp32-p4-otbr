#pragma once

// IPv6 address text (RFC 5952), shared by the settings JSON and the metrics labels. Pure, host-tested.

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ROUTING_IPV6_TEXT_MAX 40 /**< "xxxx:" * 8, the last colon replaced by the terminator */

/**
 * `addr` as RFC 5952 text: lower case, no leading zeros, the first longest run of two or more zero groups as "::".
 * Truncated to `cap` - 1 characters; ROUTING_IPV6_TEXT_MAX always fits.
 */
void routing_ipv6_to_text(const uint8_t addr[16], char *buf, size_t cap);

#ifdef __cplusplus
}
#endif
