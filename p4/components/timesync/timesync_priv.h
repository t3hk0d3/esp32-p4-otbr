#pragma once

// Between timesync.c and dhcp_wrap.c.

#include <stdint.h>
#include "esp_netif.h"
#include "timesync.h"

/** Record what one lease names (count 0: none) and wake the timesync task. Any task, lwIP's thread included. */
void timesync_note_dhcp(uint8_t count, const uint32_t *addrs_net_order, timesync_iface_t iface);

/** Which of netcfg's interfaces this is; TIMESYNC_IFACE_NONE when none of them. */
timesync_iface_t timesync_iface_of(const esp_netif_t *netif);
