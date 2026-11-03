// Validation, defaults and change detection of the network names. Pure: no ESP-IDF calls.
#include "netnames_rules.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define NAME_MAX_LEN (NETNAMES_NAME_LEN - 1)
#define STR(x) STR_(x)
#define STR_(x) #x

/* An empty name, or a DNS label: 1-max_len of letters, digits and '-', not starting or ending with '-'. */
static bool label_valid(const char *name, size_t max_len, const char *too_long, const char **reason)
{
    const size_t len = strnlen(name, NETNAMES_NAME_LEN);

    if (len == 0) {
        return true;
    }
    if (len > max_len) {
        *reason = too_long;
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        const unsigned char c = (unsigned char)name[i];

        if (!(isascii(c) && (isalnum(c) || c == '-'))) {
            *reason = "only letters, digits and hyphens are allowed";
            return false;
        }
    }
    if (name[0] == '-' || name[len - 1] == '-') {
        *reason = "must not start or end with a hyphen";
        return false;
    }
    return true;
}

/* Length of the UTF-8 sequence that starts with `lead`, or 0 when `lead` cannot start one. */
static size_t utf8_seq_len(unsigned char lead)
{
    if (lead < 0x80) {
        return 1;
    }
    if (lead >= 0xc2 && lead <= 0xdf) {
        return 2;
    }
    if (lead >= 0xe0 && lead <= 0xef) {
        return 3;
    }
    if (lead >= 0xf0 && lead <= 0xf4) {
        return 4;
    }
    return 0;
}

static bool free_text_valid(const char *name, const char **reason)
{
    const size_t len = strnlen(name, NETNAMES_NAME_LEN);

    if (len == 0) {
        *reason = "must not be empty";
        return false;
    }
    if (len > NAME_MAX_LEN) {
        *reason = "longer than 63 bytes";
        return false;
    }
    if (name[0] == ' ' || name[len - 1] == ' ') {
        *reason = "must not start or end with a space";
        return false;
    }
    for (size_t i = 0; i < len;) {
        const unsigned char c = (unsigned char)name[i];
        const size_t n = utf8_seq_len(c);

        if (c < 0x20 || c == 0x7f) {
            *reason = "must not contain control characters";
            return false;
        }
        if (n == 0 || i + n > len) {
            *reason = "is not valid UTF-8";
            return false;
        }
        for (size_t k = 1; k < n; k++) {
            if (((unsigned char)name[i + k] & 0xc0) != 0x80) {
                *reason = "is not valid UTF-8";
                return false;
            }
        }
        // Overlong 3- and 4-byte forms, encoded surrogates and code points above U+10FFFF (the ranges json_out's
        // writer refuses too): the second byte of these lead bytes has a narrower range.
        const unsigned char b1 = n > 1 ? (unsigned char)name[i + 1] : 0;

        if ((c == 0xe0 && b1 < 0xa0) || (c == 0xed && b1 > 0x9f) || (c == 0xf0 && b1 < 0x90) || (c == 0xf4 && b1 > 0x8f)) {
            *reason = "is not valid UTF-8";
            return false;
        }
        i += n;
    }
    return true;
}

bool netnames_rules_check(const netnames_settings_t *in, const char **field, const char **reason)
{
    if (!label_valid(in->hostname, NAME_MAX_LEN, "longer than 63 characters", reason)) {
        *field = "hostname";
        return false;
    }
    if (!in->ha_name_follows_hostname && !free_text_valid(in->ha_name, reason)) {
        *field = "ha_name";
        return false;
    }
    if (!label_valid(in->dhcp_name, NETCFG_DHCP_NAME_MAX, "longer than " STR(NETCFG_DHCP_NAME_MAX) " characters", reason)) {
        *field = "dhcp_name";
        return false;
    }
    return true;
}

static void lower(char *s)
{
    for (; *s; s++) {
        *s = (char)tolower((unsigned char) * s);
    }
}

void netnames_rules_normalise(netnames_settings_t *s)
{
    lower(s->hostname);
    lower(s->dhcp_name);
}

void netnames_rules_default_hostname(const uint8_t mac[6], char out[NETNAMES_NAME_LEN])
{
    snprintf(out, NETNAMES_NAME_LEN, "esp-otbr-%02x%02x%02x", mac[3], mac[4], mac[5]);
}

void netnames_rules_effective(const netnames_settings_t *s, const char *default_hostname, char hostname[NETNAMES_NAME_LEN],
                              char ha_name[NETNAMES_NAME_LEN], char dhcp_name[NETNAMES_NAME_LEN])
{
    strlcpy(hostname, s->hostname[0] ? s->hostname : default_hostname, NETNAMES_NAME_LEN);
    strlcpy(ha_name, s->ha_name_follows_hostname ? hostname : s->ha_name, NETNAMES_NAME_LEN);
    strlcpy(dhcp_name, s->dhcp_name[0] ? s->dhcp_name : NETNAMES_DEFAULT_DHCP_NAME, NETNAMES_NAME_LEN);
}

netnames_steps_t netnames_rules_steps(const netnames_settings_t *old, const netnames_settings_t *new,
                                      const char *default_hostname, const char *live_hostname)
{
    char old_host[NETNAMES_NAME_LEN], old_ha[NETNAMES_NAME_LEN], old_dhcp[NETNAMES_NAME_LEN];
    char new_host[NETNAMES_NAME_LEN], new_ha[NETNAMES_NAME_LEN], new_dhcp[NETNAMES_NAME_LEN];
    netnames_steps_t steps;

    netnames_rules_effective(old, default_hostname, old_host, old_ha, old_dhcp);
    netnames_rules_effective(new, default_hostname, new_host, new_ha, new_dhcp);
    steps.ha_name = strcmp(old_ha, new_ha) != 0;
    // The _http._tcp instance is the HA name, so a rename of it is a step of the service too.
    steps.http_advert = old->http_advert != new->http_advert || steps.ha_name;
    steps.dhcp_name = strcmp(old_dhcp, new_dhcp) != 0;
    // A save that changes nothing else while the library uses another name (a conflict rename, or mDNS not running)
    // is a retry of the name; a save of something else leaves the renamed host alone.
    const bool other_change = steps.http_advert || steps.dhcp_name;

    steps.hostname = strcmp(old_host, new_host) != 0 || (!other_change && strcmp(live_hostname, new_host) != 0);
    return steps;
}
