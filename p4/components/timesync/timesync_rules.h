#pragma once

// Validation, server choice, state and date conversion of the time sync. Pure; host-tested in test_host/.

#include <stdbool.h>
#include <stdint.h>
#include "timesync.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Without an answer for this long after a start, the state is unreachable. */
#define TIMESYNC_ANSWER_TIMEOUT_US (60LL * 1000 * 1000)

/**
 * Are the settings valid? On false, *field is "mode" or "server" and *reason a short English reason. Server: up to 63
 * bytes in every mode, empty only in auto; else an IPv4 literal, an IPv6 literal or a hostname (labels of a-z A-Z 0-9
 * '-' separated by dots, no label starting or ending with '-').
 */
bool timesync_rules_check(const timesync_config_t *in, const char **field, const char **reason);

/** The servers to use: none when off; the custom server alone; the lease's (up to 2); else pool.ntp.org. */
void timesync_rules_choose(const timesync_config_t *cfg, const timesync_dhcp_t *dhcp, timesync_servers_t *out);

/** Same servers in the same order and the same source. */
bool timesync_rules_servers_equal(const timesync_servers_t *a, const timesync_servers_t *b);

/** The state (spec 4.2) from: enabled, time since the last start, an answer since then, any server still reachable. */
timesync_state_t timesync_rules_state(bool enabled, int64_t since_start_us, bool answered_since_start, bool any_reachable);

/** The epoch seconds of the moment at_uptime_us, given the clock now; 0 when now_epoch is not valid. */
int64_t timesync_rules_epoch_at(int64_t now_epoch, int64_t now_uptime_us, int64_t at_uptime_us);

/** Whole seconds from at_uptime_us to now_uptime_us; -1 when at_uptime_us < 0 (never). */
int64_t timesync_rules_age_s(int64_t now_uptime_us, int64_t at_uptime_us);

/** now_epoch at or after 2020-01-01. */
bool timesync_rules_clock_valid(int64_t now_epoch);

/**
 * Replace the whole record with what one lease names: the addresses (network byte order, count clipped to 2) and its
 * interface. A count of 0 clears it, interface included, unless another interface named the servers held: a lease
 * without NTP servers on one interface does not take away those of the other (split mode).
 */
void timesync_rules_dhcp_capture(timesync_dhcp_t *dst, uint8_t count, const uint32_t *addrs_net_order,
                                 timesync_iface_t iface);

#ifdef __cplusplus
}
#endif
