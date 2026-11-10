// ZLOE's mDNS records (`_zloe._udp`, TXT xa, xp, nwk, role, sec, v), as the host implementation writes them. Pure.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ZIGBEE_ZLOE_TXT_ITEMS 6

typedef struct {
    const char *key;
    const char *value;
} zigbee_zloe_txt_t;

typedef struct {
    uint8_t eui64[8]; // wire order
    uint8_t epan[8];  // wire order
    uint16_t nwk;
    bool coordinator;
} zigbee_zloe_record_t;

// The strings the built items point to.
typedef struct {
    char xa[17];
    char xp[17];
    char nwk[5];
} zigbee_zloe_txt_buf_t;

// A router's record: fills `items` (ZIGBEE_ZLOE_TXT_ITEMS of them) and `buf`.
void zigbee_zloe_txt_build(const zigbee_zloe_record_t *record, zigbee_zloe_txt_t *items, zigbee_zloe_txt_buf_t *buf);
// false: a required key is missing or malformed, or the node's security mode is not none.
bool zigbee_zloe_txt_parse(const zigbee_zloe_txt_t *items, size_t count, zigbee_zloe_record_t *out);
// Keeps the IPv6 link-local addresses of `addrs`, in order; returns how many.
size_t zigbee_zloe_keep_link_local(uint8_t (*addrs)[16], size_t count);

#ifdef __cplusplus
}
#endif
