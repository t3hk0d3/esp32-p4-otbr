#pragma once

typedef enum {
    BR_MODE_STARTING,
    BR_MODE_BR_RUNNING,
    BR_MODE_SNIFFER,
    BR_MODE_RADIO_FAULT,
    BR_MODE_NO_RADIO, // no OpenThread: no present channel is in otbr (none set, or its dongle unplugged); not a fault
} br_core_mode_t;
