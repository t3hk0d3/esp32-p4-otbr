#include "zigbee_kick.h"

void zigbee_kick_init(zigbee_kick_t *k)
{
    atomic_init(&k->pending, false);
}

bool zigbee_kick_claim(zigbee_kick_t *k)
{
    return !atomic_exchange(&k->pending, true);
}

void zigbee_kick_unclaim(zigbee_kick_t *k)
{
    atomic_store(&k->pending, false);
}

void zigbee_kick_taken(zigbee_kick_t *k)
{
    atomic_store(&k->pending, false);
}

bool zigbee_input_newer(uint32_t seen_seq, uint32_t input_seq)
{
    return (int32_t)(input_seq - seen_seq) > 0;
}
