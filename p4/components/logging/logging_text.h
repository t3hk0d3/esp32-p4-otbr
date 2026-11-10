#pragma once

// Text helpers of the logging component. Host-tested in test_host/.

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Split a syslog target `host[:port]` into a host (IPv6 without its brackets) and a port (514 when omitted).
 * Accepts IPv4, hostname, `[IPv6]` and `[IPv6]:port`. ESP_ERR_INVALID_ARG for an empty host, a port outside 1..65535,
 * an unbracketed IPv6 address, or a host that does not fit `host_cap`; the outputs are then untouched.
 */
esp_err_t logging_target_parse(const char *in, char *host, size_t host_cap, uint16_t *port);

#ifdef __cplusplus
}
#endif
