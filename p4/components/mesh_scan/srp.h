#pragma once

// Pure helpers of the SRP step: parsing, filtering and packing. No OpenThread, so the host tests build them as they are.

#include <stdbool.h>
#include <stdint.h>

#include "scan_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Read one network data service as a DNS/SRP unicast server (enterprise 44970, service data type 0x5d). The address
 * and port come from the service data (0x5d + 16 + 2 bytes) or, when the service data is 0x5d alone, from the server
 * data (16 + 2 bytes). Anycast (0x5c), other enterprises and short data give false. Only the address, port and rloc16
 * of `out` are set.
 */
bool mesh_scan_srp_server_parse(uint32_t enterprise, const uint8_t *svc, uint8_t svc_len, const uint8_t *srv,
                                uint8_t srv_len, uint16_t rloc16, mesh_scan_srp_server_t *out);

/** True when one of the addresses is under an OMR prefix of the network data or the mesh-local prefix. */
bool mesh_scan_srp_addr_kept(const mesh_scan_ip6_t *addrs, uint8_t n, const mesh_scan_prefix_t *omr, uint8_t n_omr,
                             const uint8_t ml_prefix[8]);

/**
 * Find a key in raw TXT RDATA. `val` is NULL and `val_len` 0 for a key-only entry. Stops at a malformed length. Keys
 * match case-sensitively (OTBR publishes lower-case keys).
 */
bool mesh_scan_txt_find(const uint8_t *txt, uint16_t len, const char *key, const uint8_t **val, uint8_t *val_len);

/** The longest prefix of whole TXT strings that fits `max` bytes. */
uint16_t mesh_scan_txt_cut(const uint8_t *txt, uint16_t len, uint16_t max);

/**
 * Whether a `_meshcop._udp` / `_trel._udp` record from the LAN belongs to another border router of this network:
 * TXT `xp` equals `ext_pan_id` and `xa` is present and differs from `own_ext`.
 */
bool mesh_scan_br_record_kept(const char *type, const uint8_t *txt, uint16_t len, const uint8_t ext_pan_id[8],
                              const uint8_t own_ext[8]);

/** Append bytes to the result's pool. False, with nothing changed, when they do not fit MESH_SCAN_POOL_SIZE. */
bool mesh_scan_pool_put(mesh_scan_result_t *r, const uint8_t *data, uint16_t len, uint16_t *off);

#ifdef __cplusplus
}
#endif
