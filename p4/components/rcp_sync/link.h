#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "rcp_sync.h"

#ifdef __cplusplus
extern "C" {
#endif

/** The C6 side of a sync session. Production: SPI master (link_spi.c); host tests: a scripted fake. */
typedef struct {
    /** Hardware-reset the C6 into its updater (GPIO6 low, GPIO54 pulse) and bring the link up. */
    esp_err_t (*enter_updater)(void);
    /**
     * One request/reply exchange: wait for the C6 to be ready, send req, wait up to timeout_ms for the
     * reply, then read up to cap bytes of it. ESP_ERR_TIMEOUT if either phase times out.
     */
    esp_err_t (*transact)(const uint8_t *req, size_t req_len, uint8_t *reply, size_t cap, size_t *reply_len,
                          uint32_t timeout_ms);
    /** Free the SPI bus and drive GPIO6 high. Safe to call when enter_updater was never called. */
    void (*release)(void);
    /** The maintenance jumper's ROM banner was seen on JUMPER_RX (latched). NULL: no jumper support. */
    bool (*jumper_present)(void);
    /** Rewrite the C6 base image over the jumper (rcp_sync_rom_recover). NULL: not available. */
    esp_err_t (*rom_recover)(void);
    /** Version text of the embedded C6 base image, NUL-terminated; NULL or "" when none is embedded. */
    const char *(*base_version)(void);
} rcp_sync_link_t;

/** rcp_sync_probe on an explicit link; stores the result for rcp_sync_get_last_probe(). */
esp_err_t rcp_sync_probe_with(const rcp_sync_link_t *link, rcp_sync_probe_result_t *out);

/** rcp_sync_push_firmware on an explicit link. */
esp_err_t rcp_sync_push_firmware_with(const rcp_sync_link_t *link, const rcp_fw_source_t *source, char *err_detail,
                                      size_t err_cap);

/** rcp_sync_rom_recover on an explicit link: releases the link, then runs link->rom_recover. ESP_ERR_NOT_SUPPORTED without one. */
esp_err_t rcp_sync_rom_recover_with(const rcp_sync_link_t *link);

/** Forget that an automatic ROM recovery ran this boot (host tests only: the guard is per boot, not per probe). */
void rcp_sync_recovery_guard_reset(void);

/** Production link: SPI2 master on the board.h pins. */
extern const rcp_sync_link_t rcp_sync_link_spi;

#ifdef __cplusplus
}
#endif
