#pragma once

// BGP message codec (spec section 5): encodes OPEN, KEEPALIVE, UPDATE (IPv6 unicast through MP_REACH_NLRI /
// MP_UNREACH_NLRI) and NOTIFICATION; decodes headers, OPEN and NOTIFICATION, and walks a received UPDATE just far
// enough to count its NLRI. RFC 4271, 4760, 2545 and 6793. Pure, host-tested against scapy-built vectors.
//
// Every `msg` argument is a whole message, header included, and `len` is its length (the header's length field, as
// routing_bgp_decode_header returned it). Encoders write a whole message and return its length, or 0 without writing
// anything when it does not fit in `cap`.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "routing_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ROUTING_BGP_HDR_LEN 19
#define ROUTING_BGP_MAX_MSG 4096 /**< RFC 4271; extended messages are not advertised */

#define ROUTING_BGP_MSG_OPEN 1
#define ROUTING_BGP_MSG_UPDATE 2
#define ROUTING_BGP_MSG_NOTIFICATION 3
#define ROUTING_BGP_MSG_KEEPALIVE 4

/** A NOTIFICATION error code and subcode (RFC 4271 section 4.5); code 0 means OK. */
typedef struct {
    uint8_t code;
    uint8_t subcode;
} routing_bgp_err_t;

/** The next-hop sent in MP_REACH_NLRI: global + link-local (32 bytes, RFC 2545), or link-local only (16 bytes). */
typedef struct {
    uint8_t global[16];
    bool has_global;
    uint8_t link_local[16];
} routing_bgp_nexthop_t;

/** What a valid OPEN negotiates. */
typedef struct {
    uint32_t asn;       /**< the 4-octet ASN capability when present, else My AS */
    uint32_t router_id; /**< host order */
    uint16_t hold_time;
    bool ipv6_unicast; /**< Multiprotocol capability AFI 2 / SAFI 1 offered */
} routing_bgp_open_t;

/**
 * OPEN: version 4, My AS = `local_asn` (AS_TRANS 23456 above 65535), hold time, BGP identifier `router_id` (host
 * order), and one Capabilities parameter: Multiprotocol AFI 2 / SAFI 1 and 4-octet ASN `local_asn`.
 */
size_t routing_bgp_encode_open(uint8_t *buf, size_t cap, uint32_t local_asn, uint16_t hold_time, uint32_t router_id);

size_t routing_bgp_encode_keepalive(uint8_t *buf, size_t cap);

/**
 * UPDATE announcing `announce` (each a /64): ORIGIN IGP; AS_PATH empty on iBGP, one AS_SEQUENCE [local_asn] (4-octet)
 * on eBGP; LOCAL_PREF 100 on iBGP only; MP_REACH_NLRI AFI 2 / SAFI 1 with `nh`. No NEXT_HOP and no IPv4 NLRI.
 * Returns 0 as well when `announce` holds more than ROUTING_MAX_ANNOUNCED prefixes.
 */
size_t routing_bgp_encode_update(uint8_t *buf, size_t cap, bool ibgp, uint32_t local_asn,
                                 const routing_bgp_nexthop_t *nh, const routing_prefix_set_t *announce);

/** UPDATE whose only path attribute is MP_UNREACH_NLRI AFI 2 / SAFI 1 listing `withdraw` (each a /64). */
size_t routing_bgp_encode_withdraw(uint8_t *buf, size_t cap, const routing_prefix_set_t *withdraw);

/** NOTIFICATION without data. */
size_t routing_bgp_encode_notification(uint8_t *buf, size_t cap, uint8_t code, uint8_t subcode);

/**
 * Checks the first ROUTING_BGP_HDR_LEN bytes of `buf` (`len` bytes available) and returns the message type and
 * length. Errors: marker not all ones 1/1; fewer than 19 bytes available, length below 19, above 4096 or below the
 * minimum of its type (OPEN 29, UPDATE 23, NOTIFICATION 21, KEEPALIVE exactly 19) 1/2; type not 1..4 1/3.
 */
routing_bgp_err_t routing_bgp_decode_header(const uint8_t *buf, size_t len, uint8_t *type, uint16_t *msg_len);

/**
 * Validates an OPEN and fills `out` (meaningful only when OK). Capabilities may be spread across several
 * Capabilities parameters; unknown capabilities are ignored. Errors: version not 4 2/1; peer ASN not
 * `expected_asn` 2/2; BGP identifier 0 2/3; a parameter other than Capabilities 2/4; hold time 1 or 2 2/6; no
 * Multiprotocol AFI 2 / SAFI 1 capability 2/7; parameters or capabilities that do not fit 2/0; shorter than 29 1/2.
 */
routing_bgp_err_t routing_bgp_decode_open(const uint8_t *msg, size_t len, uint32_t expected_asn,
                                          routing_bgp_open_t *out);

/**
 * Counts the prefixes a received UPDATE announces: the NLRI in MP_REACH_NLRI AFI 2 / SAFI 1 plus the trailing IPv4
 * NLRI. Withdrawals are not counted. Errors: withdrawn or attribute list length past the message, or an attribute
 * header or value past the attribute list 3/1; MP_REACH_NLRI too short for its next-hop 3/9; a prefix length above
 * 128 (32 for IPv4), or prefix bytes past their field 3/10; shorter than 23 1/2.
 */
routing_bgp_err_t routing_bgp_count_update_nlri(const uint8_t *msg, size_t len, uint32_t *count);

/** Reads the code and subcode of a NOTIFICATION (any data after them is ignored). Shorter than 21: 1/2. */
routing_bgp_err_t routing_bgp_decode_notification(const uint8_t *msg, size_t len, uint8_t *code, uint8_t *subcode);

#ifdef __cplusplus
}
#endif
