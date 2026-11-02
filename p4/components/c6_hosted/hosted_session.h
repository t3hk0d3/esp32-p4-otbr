#pragma once

// The detect and install sequences over a link (SDIO on the device, a scripted fake in the host tests).

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "c6_hosted.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /** Reset the C6 into its normal boot (GPIO6 high) and bring up the SDIO bus. */
    esp_err_t (*start)(void);
    /** Tell the C6 the host is ready (host interrupt "open data path"). */
    esp_err_t (*open_data_path)(void);
    /** Send one packet. */
    esp_err_t (*send)(const uint8_t *buf, size_t len, uint32_t timeout_ms);
    /** Wait up to timeout_ms for data from the C6 and read what it has (one or more whole packets). ESP_ERR_TIMEOUT if none. */
    esp_err_t (*recv)(uint8_t *buf, size_t cap, size_t *len, uint32_t timeout_ms);
    /** Free the bus and its pins. Called once on every path after start. */
    void (*release)(void);
    /** Milliseconds since some fixed point; the session's deadlines. */
    uint32_t (*now_ms)(void);
} hosted_link_t;

#define HOSTED_DETECT_TIMEOUT_MS 3000
#define HOSTED_BEGIN_TIMEOUT_MS  30000 /* the C6 erases its 1.5 MB slot before it answers */
#define HOSTED_WRITE_TIMEOUT_MS  5000
#define HOSTED_END_TIMEOUT_MS    30000 /* it verifies the image before it answers */

/** See c6_hosted_detect. */
esp_err_t hosted_session_detect(const hosted_link_t *link, bool *present);

/** See c6_hosted_install. */
esp_err_t hosted_session_install(const hosted_link_t *link, const c6_hosted_segment_t *segs, size_t count, char *err_detail,
                                 size_t err_cap);

#ifdef __cplusplus
}
#endif
