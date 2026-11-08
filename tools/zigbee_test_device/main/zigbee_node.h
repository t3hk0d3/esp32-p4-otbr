#pragma once

#include <stdbool.h>
#include "esp_err.h"

#define ZIGBEE_NODE_ON_OFF_LIGHT_EP 10

/** Start the Zigbee stack as an on/off light router in its own task; it joins an open network by itself. */
esp_err_t zigbee_node_start(void);

/** Leave the network, clear the Zigbee data and start steering again (joins the next network that permits it). */
esp_err_t zigbee_node_factory_reset(void);

/** True while the node is on a network. */
bool zigbee_node_is_joined(void);

/** Active scan on channels 11..26: prints `scan: ...` per beacon, then `scan: done`. */
esp_err_t zigbee_node_scan(void);

/** The channels steering scans from now on (a bitmask within channels 11..26; all of them at boot). */
esp_err_t zigbee_node_set_channels(uint32_t mask);
