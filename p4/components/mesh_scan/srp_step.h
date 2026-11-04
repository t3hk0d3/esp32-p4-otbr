#pragma once

// Staging of the SRP step (private to scan.c): remote records kept by the filter wait here, with their TXT in a pool
// of their own, until finish merges them after this board's snapshot. No OpenThread and no ops, so the host tests
// build it as it is.

#include <stdint.h>

#include "scan.h"
#include "scan_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Remote hosts of one scan, unique by host name (case-insensitive). Service `txt_off` values point into `pool`, not the
 * result's pool. About 108 KB: allocated by scan.c through ops.alloc, only when the network data lists a remote server.
 */
typedef struct {
    mesh_scan_srp_host_t hosts[MESH_SCAN_MAX_SRP_HOSTS];
    uint8_t from[MESH_SCAN_MAX_SRP_HOSTS]; /**< per host, one bit per server index that gave it */
    uint8_t n_hosts;
    uint8_t ext_pan_id[8]; /**< this network's, for the meshcop/TREL rule */
    uint8_t own_ext[8];    /**< this board's extended address, for the meshcop/TREL rule */
    uint16_t pool_used;
    uint8_t pool[MESH_SCAN_POOL_SIZE];
} mesh_scan_stage_t;

/**
 * Stage `rec`, answered by server `server` (an index into `r->srp_servers`) for `type`, when it passes the filter:
 * `_meshcop._udp` / `_trel._udp` by mesh_scan_br_record_kept(), any other type by mesh_scan_srp_addr_kept() with the
 * prefixes in `r->network`. A record without a host name is dropped. A new host takes its host fields (addresses,
 * ttl_s, source) from this record; a known one keeps its own and gains the service unless it has that instance and
 * type already or MESH_SCAN_MAX_SRP_SERVICES. Counts the host once in that server's `hosts`. TXT is cut to whole
 * strings within MESH_SCAN_TXT_MAX; `txt_truncated` when cut or left out for lack of pool space.
 */
void mesh_scan_stage_record(mesh_scan_stage_t *st, mesh_scan_result_t *r, uint8_t server, const char *type,
                            const mesh_scan_srp_record_t *rec);

/**
 * Merge the staged hosts into `r` after this board's snapshot: a host already in `r` (same name, case-insensitive)
 * keeps its fields and gains the services it lacks (instance and type, case-insensitive); any other is appended while
 * there is room for MESH_SCAN_MAX_SRP_HOSTS. TXT moves to the result's pool; a service whose TXT does not fit loses it
 * (`has_txt` false, `txt_truncated` true).
 */
void mesh_scan_stage_merge(const mesh_scan_stage_t *st, mesh_scan_result_t *r);

#ifdef __cplusplus
}
#endif
