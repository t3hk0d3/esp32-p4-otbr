// 802.1Q frame helpers of the driver-level demux (spec 5.13). Pure: no driver, no lwIP.
#include "netcfg_vlan.h"

#include <string.h>

#define TAG_HEADER_LEN (NETCFG_MAC_PAIR_LEN + NETCFG_VLAN_TAG_LEN) /* MAC addresses + tag, before the payload ethertype */
#define TPID_HIGH (NETCFG_VLAN_TPID >> 8)
#define TPID_LOW (NETCFG_VLAN_TPID & 0xff)
#define VID_HIGH_MASK (NETCFG_VLAN_VID_MASK >> 8)

static bool is_tagged(const uint8_t *buf, size_t len)
{
    return len > NETCFG_MAC_PAIR_LEN + 1 && buf[NETCFG_MAC_PAIR_LEN] == TPID_HIGH && buf[NETCFG_MAC_PAIR_LEN + 1] == TPID_LOW;
}

netcfg_rx_result_t netcfg_demux(const netcfg_demux_t *table, uint8_t *buf, size_t len)
{
    netcfg_rx_result_t result = {.verdict = NETCFG_RX_DROP_MALFORMED};

    if (is_tagged(buf, len)) {
        if (len < TAG_HEADER_LEN + 2) { /* the tag and the ethertype behind it must be there */
            return result;
        }
        const uint16_t vid = (uint16_t)(((buf[NETCFG_MAC_PAIR_LEN + 2] & VID_HIGH_MASK) << 8) | buf[NETCFG_MAC_PAIR_LEN + 3]);

        result.verdict = NETCFG_RX_DROP_UNKNOWN_VLAN;
        for (size_t i = 0; vid != 0 && i < table->count; i++) {
            if (table->vlan[i] == vid) {
                memmove(buf + NETCFG_VLAN_TAG_LEN, buf, NETCFG_MAC_PAIR_LEN);
                result = (netcfg_rx_result_t) {
                    NETCFG_RX_DELIVER, i, buf + NETCFG_VLAN_TAG_LEN, len - NETCFG_VLAN_TAG_LEN
                };
                break;
            }
        }
        return result;
    }
    result.verdict = NETCFG_RX_DROP_NO_UNTAGGED;
    for (size_t i = 0; i < table->count; i++) {
        if (table->vlan[i] == 0) {
            return (netcfg_rx_result_t) {
                NETCFG_RX_DELIVER, i, buf, len
            };
        }
    }
    return result;
}

void netcfg_vlan_tag(uint16_t vid, uint8_t tag[NETCFG_VLAN_TAG_LEN])
{
    tag[0] = TPID_HIGH;
    tag[1] = TPID_LOW;
    tag[2] = (uint8_t)((vid >> 8) & VID_HIGH_MASK);
    tag[3] = (uint8_t)(vid & 0xff);
}
