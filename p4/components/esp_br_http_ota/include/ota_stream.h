#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "ota_bundle.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Flash backend of the bundle stream. The stream only knows the order of the steps; the backend owns the partitions
 * (the inactive web_M and ota_M, never the running pair) so the state machine runs on the host.
 */
typedef struct {
    /** Erase the whole inactive web partition. ESP_ERR_INVALID_SIZE if web_len does not fit in it. */
    esp_err_t (*web_begin)(void *ctx, uint32_t web_len);
    /** Raw write at `offset` within web_M. */
    esp_err_t (*web_write)(void *ctx, uint32_t offset, const void *data, size_t len);
    /** Compare SHA-256 of the web_len bytes stored in web_M with `sha256`; ESP_ERR_INVALID_CRC on mismatch. */
    esp_err_t (*web_verify)(void *ctx, uint32_t web_len, const uint8_t sha256[32]);
    /** Open the ota_M write session (size unknown). */
    esp_err_t (*app_begin)(void *ctx);
    esp_err_t (*app_write)(void *ctx, const void *data, size_t len);
    /** Close the ota_M session and validate the image; the session is released even on error. */
    esp_err_t (*app_end)(void *ctx);
    /** Release an open ota_M session without validating (only called while a session is open). */
    void (*app_abort)(void *ctx);
    /** Select ota_M as boot partition: the only commit point. */
    esp_err_t (*commit)(void *ctx);
} ota_stream_ops_t;

/**
 * Whether an update may start, from the state of the device (pure; esp_br_ota_begin asks before it erases anything):
 * ESP_ERR_INVALID_STATE while a committed update waits for its reboot (the boot partition differs from the running
 * one and must not be rewritten); ESP_BR_OTA_ERR_NOT_CONFIRMED while the running image is pending verification (the
 * inactive web slot is then the rollback target's, and esp_ota_begin would refuse after the erase); else ESP_OK.
 */
esp_err_t ota_stream_start_check(bool boot_is_running, bool running_pending_verify);

typedef enum {
    OTA_STREAM_HEADER,
    OTA_STREAM_WEB,
    OTA_STREAM_APP,
    OTA_STREAM_DONE,
    OTA_STREAM_FAILED,
} ota_stream_state_t;

typedef struct {
    const ota_stream_ops_t *ops;
    void *ctx;
    ota_stream_state_t state;
    uint8_t header_buf[OTA_BUNDLE_HEADER_LEN];
    size_t header_got;
    ota_bundle_header_t header;
    uint32_t web_got;
    uint64_t app_got;
    bool app_open;
} ota_stream_t;

void ota_stream_init(ota_stream_t *stream, const ota_stream_ops_t *ops, void *ctx);

/**
 * Feed the next bytes of a bundle; any chunking is allowed (a chunk may split the header or the web/app boundary).
 * The web image is verified as soon as its last byte arrives, before anything is written to ota_M.
 * After the first error every further call returns ESP_ERR_INVALID_STATE.
 */
esp_err_t ota_stream_write(ota_stream_t *stream, const void *data, size_t len);

/**
 * End of the bundle: validate the app image and commit. ESP_ERR_INVALID_SIZE if the bundle is truncated (header, web
 * or no app bytes); ESP_ERR_INVALID_STATE after a failure or a finished stream. Commit is reached only after the
 * header was valid, the web image fully written and verified, and the app image validated.
 */
esp_err_t ota_stream_finish(ota_stream_t *stream);

/** Release backend resources of an unfinished stream. Safe to call in any state and more than once. */
void ota_stream_abort(ota_stream_t *stream);

#ifdef __cplusplus
}
#endif
