#include <stdio.h>
#include <string.h>
#include "timesync_rules.h"

static bool is_alnum(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

static bool is_hex(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

// Four decimal octets of 0-255 separated by dots.
static bool ipv4_literal(const char *s)
{
    int octets = 0;

    while (*s) {
        int value = 0, digits = 0;

        while (*s >= '0' && *s <= '9') {
            value = value * 10 + (*s++ - '0');
            if (++digits > 3 || value > 255) {
                return false;
            }
        }
        if (digits == 0 || ++octets > 4) {
            return false;
        }
        if (*s == '.') {
            if (*++s == '\0') {
                return false;
            }
        } else if (*s) {
            return false;
        }
    }
    return octets == 4;
}

// Hex groups of 1-4 digits separated by ':', at least two colons, at most one "::". A loose check: the SNTP client
// refuses what it cannot parse, and the page then says the server does not answer.
static bool ipv6_literal(const char *s)
{
    int colons = 0, group = 0;
    const char *gap = strstr(s, "::");

    if (gap && strstr(gap + 1, "::")) {
        return false;
    }
    for (; *s; s++) {
        if (*s == ':') {
            colons++;
            group = 0;
        } else if (is_hex(*s)) {
            if (++group > 4) {
                return false;
            }
        } else {
            return false;
        }
    }
    return colons >= 2 && colons <= 7;
}

// Labels of a-z A-Z 0-9 '-', 1-63 each, separated by single dots, no label starting or ending with '-'.
static bool hostname(const char *s)
{
    size_t label = 0;
    char prev = '.';

    for (; *s; prev = *s++) {
        if (*s == '.') {
            if (label == 0 || prev == '-') {
                return false;
            }
            label = 0;
        } else if (is_alnum(*s) || *s == '-') {
            if (label == 0 && *s == '-') {
                return false;
            }
            label++;
        } else {
            return false;
        }
    }
    return label > 0 && prev != '-';
}

bool timesync_rules_check(const timesync_config_t *in, const char **field, const char **reason)
{
    if (in->mode != TIMESYNC_MODE_AUTO && in->mode != TIMESYNC_MODE_CUSTOM) {
        *field = "mode";
        *reason = "must be auto or custom";
        return false;
    }
    *field = "server";
    const size_t len = strnlen(in->server, sizeof(in->server));
    if (len >= sizeof(in->server)) {
        *reason = "longer than 63 bytes";
        return false;
    }
    if (len == 0) {
        if (in->mode == TIMESYNC_MODE_CUSTOM) {
            *reason = "a custom server is required";
            return false;
        }
    } else if (!ipv4_literal(in->server) && !ipv6_literal(in->server) && !hostname(in->server)) {
        *reason = "not a hostname or an IP address";
        return false;
    }
    *field = NULL;
    *reason = NULL;
    return true;
}

void timesync_rules_choose(const timesync_config_t *cfg, const timesync_dhcp_t *dhcp, timesync_servers_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!cfg->enabled) {
        out->source = TIMESYNC_SOURCE_NONE;
    } else if (cfg->mode == TIMESYNC_MODE_CUSTOM) {
        out->count = 1;
        strlcpy(out->addr[0], cfg->server, sizeof(out->addr[0]));
        out->source = TIMESYNC_SOURCE_CUSTOM;
    } else if (dhcp->count > 0) {
        out->count = dhcp->count > TIMESYNC_MAX_SERVERS ? TIMESYNC_MAX_SERVERS : dhcp->count;
        for (uint8_t i = 0; i < out->count; i++) {
            strlcpy(out->addr[i], dhcp->addr[i], sizeof(out->addr[i]));
        }
        out->source = TIMESYNC_SOURCE_DHCP;
    } else {
        out->count = 1;
        strlcpy(out->addr[0], TIMESYNC_DEFAULT_SERVER, sizeof(out->addr[0]));
        out->source = TIMESYNC_SOURCE_DEFAULT;
    }
}

bool timesync_rules_servers_equal(const timesync_servers_t *a, const timesync_servers_t *b)
{
    if (a->count != b->count || a->source != b->source) {
        return false;
    }
    for (uint8_t i = 0; i < a->count; i++) {
        if (strcmp(a->addr[i], b->addr[i]) != 0) {
            return false;
        }
    }
    return true;
}

timesync_state_t timesync_rules_state(bool enabled, int64_t since_start_us, bool answered_since_start, bool any_reachable,
                                      int64_t since_answer_us)
{
    if (!enabled) {
        return TIMESYNC_STATE_OFF;
    }
    if (answered_since_start) {
        const bool stale = since_answer_us >= TIMESYNC_STALE_US;
        return any_reachable && !stale ? TIMESYNC_STATE_SYNCED : TIMESYNC_STATE_UNREACHABLE;
    }
    return since_start_us < TIMESYNC_ANSWER_TIMEOUT_US ? TIMESYNC_STATE_WAITING : TIMESYNC_STATE_UNREACHABLE;
}

bool timesync_rules_clock_valid(int64_t now_epoch)
{
    return now_epoch >= TIMESYNC_EPOCH_VALID;
}

int64_t timesync_rules_epoch_at(int64_t now_epoch, int64_t now_uptime_us, int64_t at_uptime_us)
{
    if (!timesync_rules_clock_valid(now_epoch)) {
        return 0;
    }
    return now_epoch - (now_uptime_us - at_uptime_us) / 1000000;
}

int64_t timesync_rules_age_s(int64_t now_uptime_us, int64_t at_uptime_us)
{
    if (at_uptime_us < 0) {
        return -1;
    }
    return (now_uptime_us - at_uptime_us) / 1000000;
}

void timesync_rules_dhcp_capture(timesync_dhcp_t *dst, uint8_t count, const uint32_t *addrs_net_order,
                                 timesync_iface_t iface)
{
    if (count == 0 && dst->count > 0 && iface != dst->iface) {
        return; // the other interface's lease names none: what this one named still stands
    }
    memset(dst, 0, sizeof(*dst));
    if (count == 0) {
        return;
    }
    dst->count = count > TIMESYNC_MAX_SERVERS ? TIMESYNC_MAX_SERVERS : count;
    for (uint8_t i = 0; i < dst->count; i++) {
        uint8_t b[4];

        memcpy(b, &addrs_net_order[i], sizeof(b));
        snprintf(dst->addr[i], sizeof(dst->addr[i]), "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
    }
    dst->iface = iface;
}

const char *timesync_mode_name(timesync_mode_t mode)
{
    return mode == TIMESYNC_MODE_CUSTOM ? "custom" : "auto";
}

const char *timesync_state_name(timesync_state_t state)
{
    switch (state) {
    case TIMESYNC_STATE_WAITING:
        return "waiting";
    case TIMESYNC_STATE_SYNCED:
        return "synced";
    case TIMESYNC_STATE_UNREACHABLE:
        return "unreachable";
    default:
        return "off";
    }
}

const char *timesync_source_name(timesync_source_t source)
{
    switch (source) {
    case TIMESYNC_SOURCE_CUSTOM:
        return "custom";
    case TIMESYNC_SOURCE_DHCP:
        return "dhcp";
    case TIMESYNC_SOURCE_DEFAULT:
        return "default";
    default:
        return NULL;
    }
}

const char *timesync_iface_name(timesync_iface_t iface)
{
    switch (iface) {
    case TIMESYNC_IFACE_SINGLE:
        return "single";
    case TIMESYNC_IFACE_MGMT:
        return "mgmt";
    case TIMESYNC_IFACE_THREAD_BB:
        return "thread_bb";
    default:
        return NULL;
    }
}
