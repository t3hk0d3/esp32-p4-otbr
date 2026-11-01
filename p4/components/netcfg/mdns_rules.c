// mDNS rules of the board (spec 5.13). Pure.
#include "netcfg_mdns.h"

#include <string.h>

#define IPV4_LEN 4
#define IPV6_LEN 16

bool netcfg_mdns_allowed(netcfg_mode_t mode, netcfg_side_t side)
{
    return mode != NETCFG_MODE_SPLIT || side == NETCFG_SIDE_THREAD;
}

// Mapped IPv6 forms become IPv4.
static netcfg_addr_t normalize(const netcfg_addr_t *addr)
{
    netcfg_addr_t out = {0};
    esp_ip4_addr_t v4;

    if (addr->is_v6 && netcfg_v4_from_mapped(addr->bytes, &v4)) {
        memcpy(out.bytes, &v4.addr, IPV4_LEN);
        return out;
    }
    return *addr;
}

static bool is_unspecified(const netcfg_addr_t *addr)
{
    const size_t len = addr->is_v6 ? IPV6_LEN : IPV4_LEN;

    for (size_t i = 0; i < len; i++) {
        if (addr->bytes[i] != 0) {
            return false;
        }
    }
    return true;
}

static bool in_list(const netcfg_addr_t *src, const netcfg_addr_t *list, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        const netcfg_addr_t candidate = normalize(&list[i]);
        const size_t len = src->is_v6 ? IPV6_LEN : IPV4_LEN;

        if (candidate.is_v6 == src->is_v6 && memcmp(candidate.bytes, src->bytes, len) == 0) {
            return true;
        }
    }
    return false;
}

bool netcfg_mdns_source_is_foreign_own(const netcfg_addr_t *src, const netcfg_addr_t *input, size_t input_count,
                                       const netcfg_addr_t *other, size_t other_count)
{
    const netcfg_addr_t source = normalize(src);

    if (is_unspecified(&source)) {
        return false;
    }
    return in_list(&source, other, other_count) && !in_list(&source, input, input_count);
}
