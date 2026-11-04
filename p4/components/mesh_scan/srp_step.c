#include "srp_step.h"

#include <string.h>
#include <strings.h>

#include "srp.h"

typedef __typeof__(((mesh_scan_srp_host_t *)0)->services[0]) service_t;

static void copy_name(char dst[MESH_SCAN_NAME_LEN], const char *src)
{
    strncpy(dst, src, MESH_SCAN_NAME_LEN - 1);
    dst[MESH_SCAN_NAME_LEN - 1] = '\0';
}

static mesh_scan_srp_host_t *find_host(mesh_scan_srp_host_t *hosts, uint8_t n, const char *name)
{
    for (uint8_t i = 0; i < n; i++) {
        if (strcasecmp(hosts[i].name, name) == 0) {
            return &hosts[i];
        }
    }
    return NULL;
}

static bool has_service(const mesh_scan_srp_host_t *h, const char *instance, const char *type)
{
    for (uint8_t i = 0; i < h->n_services; i++) {
        if (strcasecmp(h->services[i].instance, instance) == 0 && strcasecmp(h->services[i].type, type) == 0) {
            return true;
        }
    }
    return false;
}

static bool stage_put(mesh_scan_stage_t *st, const uint8_t *data, uint16_t len, uint16_t *off)
{
    if (len > MESH_SCAN_POOL_SIZE - st->pool_used) {
        return false;
    }
    memcpy(st->pool + st->pool_used, data, len);
    *off = st->pool_used;
    st->pool_used += len;
    return true;
}

static bool kept(const mesh_scan_stage_t *st, const mesh_scan_result_t *r, const char *type,
                 const mesh_scan_srp_record_t *rec, uint8_t n_addrs)
{
    if (strcmp(type, "_meshcop._udp") == 0 || strcmp(type, "_trel._udp") == 0) {
        return rec->txt && mesh_scan_br_record_kept(type, rec->txt, rec->txt_len, st->ext_pan_id, st->own_ext);
    }
    return mesh_scan_srp_addr_kept(rec->addrs, n_addrs, r->network.omr, r->network.n_omr, r->network.ml_prefix);
}

void mesh_scan_stage_record(mesh_scan_stage_t *st, mesh_scan_result_t *r, uint8_t server, const char *type,
                            const mesh_scan_srp_record_t *rec)
{
    const uint8_t n_addrs = rec->n_addrs < MESH_SCAN_MAX_SRP_ADDRS ? rec->n_addrs : MESH_SCAN_MAX_SRP_ADDRS;
    const uint16_t source = r->srp_servers[server].rloc16;

    if (rec->host[0] == '\0' || !kept(st, r, type, rec, n_addrs)) {
        return;
    }
    mesh_scan_srp_host_t *h = find_host(st->hosts, st->n_hosts, rec->host);
    if (!h) {
        if (st->n_hosts == MESH_SCAN_MAX_SRP_HOSTS) {
            return;
        }
        h = &st->hosts[st->n_hosts++];
        memset(h, 0, sizeof(*h));
        copy_name(h->name, rec->host);
        memcpy(h->addrs, rec->addrs, n_addrs * sizeof(h->addrs[0]));
        h->n_addrs = n_addrs;
        h->source = source;
        h->ttl_s = rec->ttl_s;
    }
    uint8_t *from = &st->from[h - st->hosts];
    if (!(*from & (1u << server))) {
        *from |= (uint8_t)(1u << server);
        r->srp_servers[server].hosts++;
    }
    if (has_service(h, rec->instance, type) || h->n_services == MESH_SCAN_MAX_SRP_SERVICES) {
        return;
    }
    service_t *svc = &h->services[h->n_services++];
    const uint16_t cut = rec->txt ? mesh_scan_txt_cut(rec->txt, rec->txt_len, MESH_SCAN_TXT_MAX) : 0;

    memset(svc, 0, sizeof(*svc));
    copy_name(svc->instance, rec->instance);
    copy_name(svc->type, type);
    svc->port = rec->port;
    svc->source = source;
    svc->has_txt = rec->has_txt;
    svc->txt_truncated = rec->txt_truncated || cut < rec->txt_len;
    if (cut > 0 && stage_put(st, rec->txt, cut, &svc->txt_off)) {
        svc->txt_len = cut;
    } else if (cut > 0) {
        svc->has_txt = false; // stage pool full
        svc->txt_truncated = true;
    }
}

/** Copy `in` into `out` with its TXT moved from the stage pool to the result's. */
static void merge_service(const mesh_scan_stage_t *st, mesh_scan_result_t *r, const service_t *in, service_t *out)
{
    *out = *in;
    if (in->txt_len > 0 && !mesh_scan_pool_put(r, st->pool + in->txt_off, in->txt_len, &out->txt_off)) {
        out->has_txt = false; // result pool full
        out->txt_len = 0;
        out->txt_off = 0;
        out->txt_truncated = true;
    }
}

void mesh_scan_stage_merge(const mesh_scan_stage_t *st, mesh_scan_result_t *r)
{
    for (uint8_t i = 0; i < st->n_hosts; i++) {
        const mesh_scan_srp_host_t *in = &st->hosts[i];
        mesh_scan_srp_host_t *h = find_host(r->srp, r->n_srp, in->name);

        if (!h) {
            if (r->n_srp == MESH_SCAN_MAX_SRP_HOSTS) {
                continue;
            }
            h = &r->srp[r->n_srp++];
            *h = *in;
            h->n_services = 0;
        }
        for (uint8_t j = 0; j < in->n_services && h->n_services < MESH_SCAN_MAX_SRP_SERVICES; j++) {
            if (!has_service(h, in->services[j].instance, in->services[j].type)) {
                merge_service(st, r, &in->services[j], &h->services[h->n_services++]);
            }
        }
    }
}
