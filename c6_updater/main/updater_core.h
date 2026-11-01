#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "rcp_sync_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Flash side of the updater; the device implements it with esp_ota_*, host tests with a fake. */
typedef struct {
    /** Version of the boot slot (boot_slot true) or the other slot. ESP_ERR_NOT_FOUND if empty or unverifiable. */
    esp_err_t (*slot_version)(bool boot_slot, char *out, size_t len);
    /** Erase the inactive slot and prepare to receive image_size bytes. */
    esp_err_t (*begin)(uint32_t image_size);
    /** Write len bytes at offset (sequential) into the inactive slot. */
    esp_err_t (*write)(uint32_t offset, const void *data, size_t len);
    /** Verify the written image and make its slot the boot slot. Boot slot unchanged on failure. */
    esp_err_t (*finish)(void);
    uint32_t slot_size;
} updater_flash_t;

/** Which slot the OTA data selects. */
typedef enum {
    UPDATER_OTADATA_NONE = 0,  /* blank or invalid: no slot selected */
    UPDATER_OTADATA_SLOT_0,
    UPDATER_OTADATA_SLOT_1,
} updater_otadata_t;

/**
 * The slot (0 or 1) the ESP-IDF bootloader boots: the selected slot when it holds a valid image, otherwise the
 * first valid one (ota_0, then ota_1; with a selection of ota_1: ota_1, then ota_0). When no slot holds a
 * valid image the answer is the selected slot, ota_0 without a selection.
 * The inactive slot (the next push target) is the other one.
 */
int updater_core_choose_boot_slot(updater_otadata_t selected, bool valid0, bool valid1);

typedef struct {
    const updater_flash_t *flash;
    const char *updater_version;
    bool receiving;         /* BEGIN accepted, END not yet done */
    uint32_t image_size;
    uint32_t next_offset;
    bool have_last;         /* a reply is cached for last_seq */
    uint8_t last_seq;
    uint8_t last_type;      /* identity of the request the cache answers: seq + type + len + CRC */
    uint16_t last_len;
    uint32_t last_crc;
    uint8_t cache[RCP_SYNC_PROTO_HEADER_LEN + RCP_SYNC_PROTO_HELLO_REPLY_LEN + RCP_SYNC_PROTO_CRC_LEN];
    size_t cache_len;
} updater_core_t;

void updater_core_init(updater_core_t *ctx, const updater_flash_t *flash, const char *updater_version);

/**
 * Process one request frame and write the reply frame to reply.
 * Any frame that does not decode (bad CRC, truncated, wrong magic) yields a NAK reply.
 * ESP_ERR_INVALID_ARG on NULL args; an undersized reply buffer is returned as an error with reply_len 0.
 * A retransmit (same seq, type, len and CRC as the previous non-HELLO request) is answered from the
 * cache without executing; a different request reusing a seq is executed. HELLO is always executed.
 *
 * P4-side contract (Task 8 implements it):
 *  - Hardware-reset the C6 (GPIO54, GPIO6 held low) at the start of every session; the core keeps
 *    no state across resets and has no abort command.
 *  - Wait for INT low before every request and again before clocking every reply; the reply
 *    phase must be clocked within 1 s of INT going low or the C6 drops the reply and returns
 *    to waiting for a request.
 *  - Use a fresh seq for every new request; on NAK, timeout or a dropped reply resend the identical
 *    frame (same seq) so the retransmit is idempotent.
 *  - Reply timeouts: 30 s for BEGIN (erase) and END (verify), 1 s for HELLO and DATA.
 *  - A BEGIN restarts the session (the previous partial image is discarded).
 */
esp_err_t updater_core_handle(updater_core_t *ctx, const uint8_t *req, size_t req_len,
                              uint8_t *reply, size_t reply_cap, size_t *reply_len);

#ifdef __cplusplus
}
#endif
