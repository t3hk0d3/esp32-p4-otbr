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

bool zigbee_gone_lost(uint32_t detached, uint32_t attached, uint32_t gone)
{
    return attached != 0 && detached == attached && gone != attached;
}
