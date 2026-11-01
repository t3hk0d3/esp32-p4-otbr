#pragma once

// 802.1Q frame helpers of the driver-level VLAN demux (spec 5.13). Pure; host-tested in test_host/.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NETCFG_MAX_IFACES 2
#define NETCFG_MAC_PAIR_LEN 12 /* destination + source MAC */
#define NETCFG_VLAN_TAG_LEN 4
#define NETCFG_VLAN_TPID 0x8100
#define NETCFG_VLAN_VID_MASK 0x0fff

typedef enum {
    NETCFG_RX_DELIVER,
    NETCFG_RX_DROP_UNKNOWN_VLAN, /**< tagged with a VLAN ID no interface has (also VLAN ID 0) */
    NETCFG_RX_DROP_NO_UNTAGGED,  /**< untagged, and no interface is untagged */
    NETCFG_RX_DROP_MALFORMED,    /**< a tag header cut short */
} netcfg_rx_verdict_t;

typedef struct {
    size_t count;
    uint16_t vlan[NETCFG_MAX_IFACES]; /**< VLAN ID per interface, 0 = untagged */
} netcfg_demux_t;

typedef struct {
    netcfg_rx_verdict_t verdict;
    size_t iface;     /**< NETCFG_RX_DELIVER: index into the table */
    uint8_t *frame;   /**< NETCFG_RX_DELIVER: the frame to hand to the interface, tag removed */
    size_t len;
} netcfg_rx_result_t;

/**
 * Pick the interface for a received frame. A matching tagged frame is rewritten in place: the two MAC addresses move 4
 * bytes forward over the tag, so `frame` of the result is `buf + 4` (an interface buffer must be freed at `frame - 4`).
 * An untagged frame is passed through unchanged.
 */
netcfg_rx_result_t netcfg_demux(const netcfg_demux_t *table, uint8_t *buf, size_t len);

/** The 4-byte tag (TPID 0x8100, priority 0, VLAN ID `vid`) a tagged interface inserts after the MAC addresses. */
void netcfg_vlan_tag(uint16_t vid, uint8_t tag[NETCFG_VLAN_TAG_LEN]);

#ifdef __cplusplus
}
#endif
