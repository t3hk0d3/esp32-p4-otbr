// Network configuration: defaults, validation and the JSON form (spec 5.13). No hardware, no lwIP: host-tested.
#include "netcfg_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "netcfg_priv.h"

#define FIELD_MODE "mode"
#define FIELD_VLAN "vlan"
#define FIELD_IPV4 "ipv4"
#define FIELD_ADDR "ipv4.addr"
#define FIELD_MASK "ipv4.mask"
#define FIELD_GW "ipv4.gw"
#define FIELD_DNS "ipv4.dns"
#define FIELD_JSON "json"
#define FIELD_TREL "trel"
#define SECTION_SINGLE "single"
#define SECTION_MGMT "mgmt"
#define SECTION_THREAD_BB "thread_bb"
#define MODE_SINGLE "single"
#define MODE_SPLIT "split"
#define OCTETS 4
#define OCTET_MAX 255
#define BITS_PER_OCTET 8
#define MULTICAST_FIRST 224 /* first octet of the multicast and reserved ranges (224.0.0.0/3) */
#define LOOPBACK_FIRST 127
#define ADDR_TEXT_MAX 16

/* ---- IPv4 helpers: host-order 32-bit values, independent of the byte order of esp_ip4_addr_t ---- */

static uint32_t to_host(esp_ip4_addr_t a)
{
    const uint8_t *b = (const uint8_t *)&a.addr;

    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
}

static esp_ip4_addr_t from_host(uint32_t v)
{
    esp_ip4_addr_t a;
    uint8_t *b = (uint8_t *)&a.addr;

    b[0] = (uint8_t)(v >> 24);
    b[1] = (uint8_t)(v >> 16);
    b[2] = (uint8_t)(v >> 8);
    b[3] = (uint8_t)v;
    return a;
}

static bool mask_contiguous(uint32_t mask)
{
    const uint32_t inverted = ~mask;

    return mask != 0 && (inverted & (inverted + 1)) == 0;
}

static bool usable_unicast(uint32_t a)
{
    const uint32_t first = a >> 24;

    return a != 0 && first < MULTICAST_FIRST && first != LOOPBACK_FIRST;
}

/* Strict dotted quad: four decimal fields 0..255, no sign, no spaces, no leading zeros. */
static bool parse_ipv4(const char *text, uint32_t *out)
{
    uint32_t value = 0;

    for (int octet = 0; octet < OCTETS; octet++) {
        if (*text < '0' || *text > '9') {
            return false;
        }
        const char *start = text;
        uint32_t n = 0;

        while (*text >= '0' && *text <= '9' && text - start < 4) {
            n = n * 10 + (uint32_t)(*text - '0');
            text++;
        }
        if (n > OCTET_MAX || (text - start > 1 && *start == '0')) {
            return false;
        }
        value = (value << BITS_PER_OCTET) | n;
        if (octet < OCTETS - 1) {
            if (*text != '.') {
                return false;
            }
            text++;
        }
    }
    *out = value;
    return *text == '\0';
}

static void format_ipv4(uint32_t v, char *buf, size_t cap)
{
    snprintf(buf, cap, "%u.%u.%u.%u", (unsigned)(v >> 24), (unsigned)((v >> 16) & OCTET_MAX), (unsigned)((v >> 8) & OCTET_MAX),
             (unsigned)(v & OCTET_MAX));
}

/* ---- defaults, validation ---- */

static void iface_defaults(netcfg_iface_cfg_t *iface)
{
    memset(iface, 0, sizeof(*iface));
    iface->vlan = NETCFG_VLAN_UNTAGGED;
    iface->dhcp = true;
}

void netcfg_defaults(netcfg_config_t *out)
{
    memset(out, 0, sizeof(*out));
    out->mode = NETCFG_MODE_SINGLE;
    iface_defaults(&out->single);
    iface_defaults(&out->mgmt);
    iface_defaults(&out->thread_bb);
    out->trel = true;
}

static esp_err_t fail(const char **bad_field, const char *field)
{
    if (bad_field) {
        *bad_field = field;
    }
    return ESP_ERR_INVALID_ARG;
}

static esp_err_t validate_iface(const netcfg_iface_cfg_t *iface, const char **bad_field)
{
    if (iface->vlan > NETCFG_VLAN_MAX) {
        return fail(bad_field, FIELD_VLAN);
    }
    if (iface->dhcp) {
        return ESP_OK;
    }
    const uint32_t addr = to_host(iface->addr);
    const uint32_t mask = to_host(iface->mask);

    if (!usable_unicast(addr)) {
        return fail(bad_field, FIELD_ADDR);
    }
    if (!mask_contiguous(mask) || mask == UINT32_MAX) {
        return fail(bad_field, FIELD_MASK);
    }
    const uint32_t host_bits = ~mask;

    if ((addr & host_bits) == 0 || (addr & host_bits) == host_bits) { /* network or broadcast address */
        return fail(bad_field, FIELD_ADDR);
    }
    const uint32_t gw = to_host(iface->gw);

    if (gw != 0 && (!usable_unicast(gw) || gw == addr || (gw & mask) != (addr & mask) || (gw & host_bits) == 0 ||
                    (gw & host_bits) == host_bits)) {
        return fail(bad_field, FIELD_GW);
    }
    const uint32_t dns = to_host(iface->dns);

    if (dns != 0 && !usable_unicast(dns)) {
        return fail(bad_field, FIELD_DNS);
    }
    return ESP_OK;
}

static bool same_subnet(const netcfg_iface_cfg_t *a, const netcfg_iface_cfg_t *b)
{
    const uint32_t mask = to_host(a->mask) & to_host(b->mask); /* the shorter prefix */

    return (to_host(a->addr) & mask) == (to_host(b->addr) & mask);
}

esp_err_t netcfg_validate(const netcfg_config_t *cfg, const char **bad_field)
{
    if (cfg->mode == NETCFG_MODE_SINGLE) {
        return validate_iface(&cfg->single, bad_field);
    }
    if (cfg->mode != NETCFG_MODE_SPLIT) {
        return fail(bad_field, FIELD_MODE);
    }
    esp_err_t err = validate_iface(&cfg->mgmt, bad_field);

    if (err != ESP_OK) {
        return err;
    }
    err = validate_iface(&cfg->thread_bb, bad_field);
    if (err != ESP_OK) {
        return err;
    }
    if (cfg->mgmt.vlan == cfg->thread_bb.vlan) { /* also covers two untagged interfaces */
        return fail(bad_field, FIELD_VLAN);
    }
    if (!cfg->mgmt.dhcp && !cfg->thread_bb.dhcp && same_subnet(&cfg->mgmt, &cfg->thread_bb)) {
        return fail(bad_field, FIELD_ADDR);
    }
    return ESP_OK;
}

static bool iface_equal(const netcfg_iface_cfg_t *a, const netcfg_iface_cfg_t *b)
{
    if (a->vlan != b->vlan || a->dhcp != b->dhcp) {
        return false;
    }
    return a->dhcp || (a->addr.addr == b->addr.addr && a->mask.addr == b->mask.addr && a->gw.addr == b->gw.addr &&
                       a->dns.addr == b->dns.addr);
}

// The interfaces only: mode and the interfaces that mode uses.
static bool ifaces_equal(const netcfg_config_t *a, const netcfg_config_t *b)
{
    if (a->mode != b->mode) {
        return false;
    }
    if (a->mode == NETCFG_MODE_SINGLE) {
        return iface_equal(&a->single, &b->single);
    }
    return iface_equal(&a->mgmt, &b->mgmt) && iface_equal(&a->thread_bb, &b->thread_bb);
}

bool netcfg_equal(const netcfg_config_t *a, const netcfg_config_t *b)
{
    return a->trel == b->trel && ifaces_equal(a, b);
}

bool netcfg_diff_only_trel(const netcfg_config_t *a, const netcfg_config_t *b)
{
    return a->trel != b->trel && ifaces_equal(a, b);
}

/* ---- JSON ---- */

static esp_err_t parse_addr_field(const cJSON *ipv4, const char *key, bool required, const char *field, uint32_t *out,
                                  const char **bad_field)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(ipv4, key);

    if (!item) {
        *out = 0;
        return required ? fail(bad_field, field) : ESP_OK;
    }
    if (!cJSON_IsString(item) || !parse_ipv4(item->valuestring, out)) {
        return fail(bad_field, field);
    }
    return ESP_OK;
}

static esp_err_t parse_iface(const cJSON *section, netcfg_iface_cfg_t *out, const char **bad_field)
{
    iface_defaults(out);
    const cJSON *vlan = cJSON_GetObjectItemCaseSensitive(section, FIELD_VLAN);

    // Required: a missing key must not silently mean "untagged" (a typo would drop the VLAN).
    if (!cJSON_IsNumber(vlan) || vlan->valuedouble < 0 || vlan->valuedouble > NETCFG_VLAN_MAX ||
            vlan->valuedouble != (double)(int)vlan->valuedouble) {
        return fail(bad_field, FIELD_VLAN);
    }
    out->vlan = (uint16_t)vlan->valueint;
    const cJSON *ipv4 = cJSON_GetObjectItemCaseSensitive(section, FIELD_IPV4);
    const cJSON *dhcp = cJSON_IsObject(ipv4) ? cJSON_GetObjectItemCaseSensitive(ipv4, "dhcp") : NULL;

    if (!cJSON_IsBool(dhcp)) {
        return fail(bad_field, FIELD_IPV4);
    }
    out->dhcp = cJSON_IsTrue(dhcp);
    if (out->dhcp) {
        return ESP_OK;
    }
    uint32_t addr, mask, gw, dns;
    esp_err_t err = parse_addr_field(ipv4, "addr", true, FIELD_ADDR, &addr, bad_field);

    if (err == ESP_OK) {
        err = parse_addr_field(ipv4, "mask", true, FIELD_MASK, &mask, bad_field);
    }
    if (err == ESP_OK) {
        err = parse_addr_field(ipv4, "gw", false, FIELD_GW, &gw, bad_field);
    }
    if (err == ESP_OK) {
        err = parse_addr_field(ipv4, "dns", false, FIELD_DNS, &dns, bad_field);
    }
    if (err != ESP_OK) {
        return err;
    }
    out->addr = from_host(addr);
    out->mask = from_host(mask);
    out->gw = from_host(gw);
    out->dns = from_host(dns);
    return ESP_OK;
}

static esp_err_t parse_section(const cJSON *root, const char *name, netcfg_iface_cfg_t *out, const char **bad_field)
{
    const cJSON *section = cJSON_GetObjectItemCaseSensitive(root, name);

    if (!cJSON_IsObject(section)) {
        return fail(bad_field, name);
    }
    return parse_iface(section, out, bad_field);
}

static esp_err_t parse_trel(const cJSON *root, bool *out, const char **bad_field)
{
    const cJSON *trel = cJSON_GetObjectItemCaseSensitive(root, FIELD_TREL);

    if (!trel) {
        return ESP_OK; /* keeps the value the caller put in *out */
    }
    if (!cJSON_IsBool(trel)) {
        return fail(bad_field, FIELD_TREL);
    }
    *out = cJSON_IsTrue(trel);
    return ESP_OK;
}

esp_err_t netcfg_from_json(const char *json, netcfg_config_t *out, const char **bad_field)
{
    return netcfg_from_json_over(json, NULL, out, bad_field);
}

esp_err_t netcfg_from_json_over(const char *json, const netcfg_config_t *base, netcfg_config_t *out, const char **bad_field)
{
    const bool trel_default = base ? base->trel : true;

    netcfg_defaults(out);
    out->trel = trel_default;
    if (!json || strnlen(json, NETCFG_JSON_MAX + 1) > NETCFG_JSON_MAX) {
        return fail(bad_field, FIELD_JSON);
    }
    cJSON *root = cJSON_ParseWithOpts(json, NULL, true); /* trailing text is an error */

    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return fail(bad_field, FIELD_JSON);
    }
    esp_err_t err = ESP_OK;
    const cJSON *mode = cJSON_GetObjectItemCaseSensitive(root, FIELD_MODE);

    if (cJSON_IsString(mode) && strcmp(mode->valuestring, MODE_SINGLE) == 0) {
        out->mode = NETCFG_MODE_SINGLE;
        err = parse_section(root, SECTION_SINGLE, &out->single, bad_field);
    } else if (cJSON_IsString(mode) && strcmp(mode->valuestring, MODE_SPLIT) == 0) {
        out->mode = NETCFG_MODE_SPLIT;
        err = parse_section(root, SECTION_MGMT, &out->mgmt, bad_field);
        if (err == ESP_OK) {
            err = parse_section(root, SECTION_THREAD_BB, &out->thread_bb, bad_field);
        }
    } else {
        err = fail(bad_field, FIELD_MODE);
    }
    if (err == ESP_OK) {
        err = parse_trel(root, &out->trel, bad_field);
    }
    cJSON_Delete(root);
    return err == ESP_OK ? netcfg_validate(out, bad_field) : err;
}

static cJSON *iface_to_cjson(const netcfg_iface_cfg_t *iface)
{
    cJSON *section = cJSON_CreateObject();
    cJSON *ipv4 = cJSON_CreateObject();
    char text[ADDR_TEXT_MAX];

    if (!section || !ipv4) {
        cJSON_Delete(section);
        cJSON_Delete(ipv4);
        return NULL;
    }
    cJSON_AddNumberToObject(section, FIELD_VLAN, iface->vlan);
    cJSON_AddBoolToObject(ipv4, "dhcp", iface->dhcp);
    if (!iface->dhcp) {
        format_ipv4(to_host(iface->addr), text, sizeof(text));
        cJSON_AddStringToObject(ipv4, "addr", text);
        format_ipv4(to_host(iface->mask), text, sizeof(text));
        cJSON_AddStringToObject(ipv4, "mask", text);
        format_ipv4(to_host(iface->gw), text, sizeof(text));
        cJSON_AddStringToObject(ipv4, "gw", text);
        format_ipv4(to_host(iface->dns), text, sizeof(text));
        cJSON_AddStringToObject(ipv4, "dns", text);
    }
    cJSON_AddItemToObject(section, FIELD_IPV4, ipv4);
    return section;
}

cJSON *netcfg_config_to_cjson(const netcfg_config_t *cfg)
{
    cJSON *root = cJSON_CreateObject();
    bool ok = root != NULL;

    if (!ok) {
        return NULL;
    }
    ok = cJSON_AddStringToObject(root, FIELD_MODE, cfg->mode == NETCFG_MODE_SPLIT ? MODE_SPLIT : MODE_SINGLE) != NULL;
    if (cfg->mode == NETCFG_MODE_SINGLE) {
        ok = ok && cJSON_AddItemToObject(root, SECTION_SINGLE, iface_to_cjson(&cfg->single));
    } else {
        ok = ok && cJSON_AddItemToObject(root, SECTION_MGMT, iface_to_cjson(&cfg->mgmt)) &&
             cJSON_AddItemToObject(root, SECTION_THREAD_BB, iface_to_cjson(&cfg->thread_bb));
    }
    ok = ok && cJSON_AddBoolToObject(root, FIELD_TREL, cfg->trel) != NULL;
    if (!ok) {
        cJSON_Delete(root);
        return NULL;
    }
    return root;
}

char *netcfg_to_json(const netcfg_config_t *cfg)
{
    cJSON *root = netcfg_config_to_cjson(cfg);
    char *text = root ? cJSON_PrintUnformatted(root) : NULL;

    cJSON_Delete(root);
    return text;
}
