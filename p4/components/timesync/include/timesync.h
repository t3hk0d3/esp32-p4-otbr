#pragma once

// Wall-clock time from NTP (spec docs/superpowers/specs/2026-11-10-time-sync-design.md): a custom server, else the
// servers the DHCP lease names, else pool.ntp.org. The board keeps UTC; nothing survives a power loss.

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Buffer size of a server name or address: up to 63 bytes plus the terminator. */
#define TIMESYNC_SERVER_LEN 64
/** Servers in use at most (CONFIG_LWIP_SNTP_MAX_SERVERS); DHCP servers beyond these are ignored. */
#define TIMESYNC_MAX_SERVERS 2
/** 2020-01-01 00:00:00 UTC: an earlier clock was never set. */
#define TIMESYNC_EPOCH_VALID 1577836800
/** The server of mode auto when the lease names none. */
#define TIMESYNC_DEFAULT_SERVER "pool.ntp.org"
/** Buffer size of a dotted IPv4 address. */
#define TIMESYNC_IPV4_LEN 16

typedef enum { TIMESYNC_MODE_AUTO, TIMESYNC_MODE_CUSTOM } timesync_mode_t;

typedef enum {
    TIMESYNC_SOURCE_NONE,    /**< sync is off */
    TIMESYNC_SOURCE_CUSTOM,  /**< the server set by the user */
    TIMESYNC_SOURCE_DHCP,    /**< the servers the lease names */
    TIMESYNC_SOURCE_DEFAULT, /**< pool.ntp.org */
} timesync_source_t;

typedef enum {
    TIMESYNC_STATE_OFF,
    TIMESYNC_STATE_WAITING,     /**< no answer yet, under a minute since start */
    TIMESYNC_STATE_SYNCED,      /**< answered, and a server in use is still reachable */
    TIMESYNC_STATE_UNREACHABLE, /**< no answer for a minute, or every server stopped answering */
} timesync_state_t;

/** The interface whose lease named the NTP servers. */
typedef enum {
    TIMESYNC_IFACE_NONE, /**< none, or not known */
    TIMESYNC_IFACE_SINGLE,
    TIMESYNC_IFACE_MGMT,
    TIMESYNC_IFACE_THREAD_BB,
} timesync_iface_t;

/** What the user sets. */
typedef struct {
    bool enabled;
    timesync_mode_t mode;
    char server[TIMESYNC_SERVER_LEN]; /**< the custom server; may be empty in auto */
} timesync_config_t;

/** The servers in use and where they came from. */
typedef struct {
    uint8_t count;
    char addr[TIMESYNC_MAX_SERVERS][TIMESYNC_SERVER_LEN];
    timesync_source_t source;
} timesync_servers_t;

/** What the DHCP lease names (whatever the mode). */
typedef struct {
    uint8_t count;
    char addr[TIMESYNC_MAX_SERVERS][TIMESYNC_IPV4_LEN];
    timesync_iface_t iface;
} timesync_dhcp_t;

/** A snapshot for the API. */
typedef struct {
    timesync_config_t config;
    timesync_state_t state;
    int64_t now;          /**< epoch seconds; 0 when the clock is not valid */
    int64_t synced_age_s; /**< seconds since the last answer; -1 when none since boot */
    timesync_servers_t servers;
    int8_t answered; /**< index in servers of the one that answered last; -1 when none */
    timesync_dhcp_t dhcp;
} timesync_status_t;

/** Load the settings (missing = enabled, auto) and start. Call once the network interfaces exist. */
esp_err_t timesync_init(void);

/** True when the clock is at or after 2020-01-01. Any task (not an ISR); takes no lock and never blocks. */
bool timesync_clock_valid(void);

/** The date of a past moment given as esp_timer_get_time(): epoch seconds, 0 when the clock is not valid. */
int64_t timesync_epoch_at(int64_t uptime_us);

/** The stored settings. */
void timesync_get_config(timesync_config_t *out);

/**
 * Validate, store and apply live. ESP_ERR_INVALID_ARG with *field ("enabled", "mode", "server") and *reason; a
 * storage error is returned with nothing changed.
 */
esp_err_t timesync_set_config(const timesync_config_t *in, const char **field, const char **reason);

/** A snapshot of the settings, the state and the servers. */
void timesync_get_status(timesync_status_t *out);

/** API names (spec 5.2): "auto" / "custom". */
const char *timesync_mode_name(timesync_mode_t mode);
/** "off", "waiting", "synced", "unreachable". */
const char *timesync_state_name(timesync_state_t state);
/** "custom", "dhcp", "default"; NULL for none. */
const char *timesync_source_name(timesync_source_t source);
/** "single", "mgmt", "thread_bb"; NULL for none. */
const char *timesync_iface_name(timesync_iface_t iface);

#ifdef __cplusplus
}
#endif
