#include "srp.h"

#include <string.h>

#define ENTERPRISE_THREAD 44970
#define SERVICE_DNS_SRP_UNICAST 0x5d
#define ADDR_LEN 16
#define ADDR_PORT_LEN (ADDR_LEN + 2)

bool mesh_scan_srp_server_parse(uint32_t enterprise, const uint8_t *svc, uint8_t svc_len, const uint8_t *srv,
                                uint8_t srv_len, uint16_t rloc16, mesh_scan_srp_server_t *out)
{
    const uint8_t *ap;

    if (enterprise != ENTERPRISE_THREAD || svc == NULL || svc_len < 1 || svc[0] != SERVICE_DNS_SRP_UNICAST) {
        return false;
    }
    if (svc_len == 1 + ADDR_PORT_LEN) {
        ap = svc + 1;
    } else if (svc_len == 1 && srv != NULL && srv_len >= ADDR_PORT_LEN) {
        ap = srv;
    } else {
        return false;
    }
    memcpy(out->addr, ap, ADDR_LEN);
    out->port = (uint16_t)(ap[ADDR_LEN] << 8 | ap[ADDR_LEN + 1]);
    out->rloc16 = rloc16;
    return true;
}

bool mesh_scan_srp_addr_kept(const mesh_scan_ip6_t *addrs, uint8_t n, const mesh_scan_prefix_t *omr, uint8_t n_omr,
                             const uint8_t ml_prefix[8])
{
    for (uint8_t i = 0; i < n; i++) {
        if (memcmp(addrs[i], ml_prefix, 8) == 0) {
            return true;
        }
        for (uint8_t j = 0; j < n_omr; j++) {
            if (memcmp(addrs[i], omr[j], 8) == 0) {
                return true;
            }
        }
    }
    return false;
}

bool mesh_scan_txt_find(const uint8_t *txt, uint16_t len, const char *key, const uint8_t **val, uint8_t *val_len)
{
    size_t klen = strlen(key);

    for (uint16_t i = 0; i < len;) {
        uint8_t n = txt[i];

        if (i + 1 + n > len) {
            return false;
        }
        const uint8_t *e = txt + i + 1;

        if (n >= klen && memcmp(e, key, klen) == 0 && (n == klen || e[klen] == '=')) {
            if (n == klen) {
                *val = NULL;
                *val_len = 0;
            } else {
                *val = e + klen + 1;
                *val_len = (uint8_t)(n - klen - 1);
            }
            return true;
        }
        i += 1 + n;
    }
    return false;
}

uint16_t mesh_scan_txt_cut(const uint8_t *txt, uint16_t len, uint16_t max)
{
    uint16_t i = 0;

    while (i < len) {
        uint16_t next = i + 1 + txt[i];

        if (next > len || next > max) {
            break;
        }
        i = next;
    }
    return i;
}

bool mesh_scan_br_record_kept(const char *type, const uint8_t *txt, uint16_t len, const uint8_t ext_pan_id[8],
                              const uint8_t own_ext[8])
{
    const uint8_t *xp, *xa;
    uint8_t xp_len, xa_len;

    if (strcmp(type, "_meshcop._udp") != 0 && strcmp(type, "_trel._udp") != 0) {
        return false;
    }
    if (!mesh_scan_txt_find(txt, len, "xp", &xp, &xp_len) || xp_len != 8 || memcmp(xp, ext_pan_id, 8) != 0) {
        return false;
    }
    return mesh_scan_txt_find(txt, len, "xa", &xa, &xa_len) && xa_len == 8 && memcmp(xa, own_ext, 8) != 0;
}

bool mesh_scan_pool_put(mesh_scan_result_t *r, const uint8_t *data, uint16_t len, uint16_t *off)
{
    if (r->pool == NULL || len > MESH_SCAN_POOL_SIZE - r->pool_used) {
        return false;
    }
    if (len > 0) {
        memcpy(r->pool + r->pool_used, data, len);
    }
    *off = r->pool_used;
    r->pool_used += len;
    return true;
}
