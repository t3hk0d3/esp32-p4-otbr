#include <stdatomic.h>
#include "device_api_priv.h"

// OTA and the firmware push share one slot (they must not overlap); a pcap stream has its own.
static _Atomic int s_long_op = -1; // device_api_op_t holding the shared slot, -1 = free
static atomic_bool s_pcap;

bool device_api_op_try_begin(device_api_op_t op)
{
    if (op == DEVICE_API_OP_PCAP) {
        bool expected = false;
        return atomic_compare_exchange_strong(&s_pcap, &expected, true);
    }
    int expected = -1;
    return atomic_compare_exchange_strong(&s_long_op, &expected, (int)op);
}

void device_api_op_end(device_api_op_t op)
{
    if (op == DEVICE_API_OP_PCAP) {
        atomic_store(&s_pcap, false);
        return;
    }
    int expected = (int)op;
    atomic_compare_exchange_strong(&s_long_op, &expected, -1); // only the holder releases
}

const char *device_api_op_busy_detail(device_api_op_t op)
{
    if (op == DEVICE_API_OP_PCAP) {
        return "a capture stream is already running";
    }
    switch (atomic_load(&s_long_op)) {
    case DEVICE_API_OP_OTA:
        return "an update is running or waits for its reboot";
    case DEVICE_API_OP_RADIO_PUSH:
        return "a radio firmware push is running";
    case DEVICE_API_OP_ROM_RECOVER:
        return "a ROM recovery is running";
    default:
        return "another long operation is running";
    }
}

bool device_api_op_long_running(void)
{
    return atomic_load(&s_long_op) != -1;
}
