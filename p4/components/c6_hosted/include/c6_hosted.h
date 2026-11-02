#pragma once

// The factory firmware of the on-board C6 is ESP-Hosted (esp-hosted-mcu, SDIO). This component speaks just enough of its
// protocol to recognise it and to send one image through its OTA (OTABegin / OTAWrite / OTAEnd); the wire format is the
// same from esp_hosted 0.0.6 through 2.x. It never runs on its own: rcp_sync calls it while it owns the C6 pins.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** One piece of the image sent through the OTA. data NULL: `len` bytes of 0xFF (padding). */
typedef struct {
    const uint8_t *data;
    size_t len;
} c6_hosted_segment_t;

/**
 * Reset the C6 into its normal boot (GPIO6 high) and see whether ESP-Hosted answers on SDIO: present is true when it sent
 * its startup event with the chip ID of an ESP32-C6 within a few seconds. Writes nothing to the C6. Takes and releases the
 * SDIO bus; the pins are free for the SPI link afterwards. ESP_OK whether or not it answered; an error only when the bus
 * itself could not be set up.
 */
esp_err_t c6_hosted_detect(bool *present);

/**
 * Send the segments back to back as one image through the ESP-Hosted OTA. The C6 erases its other app slot (seconds),
 * stores the image, checks it, selects it to boot and restarts by itself about 5 s after the reply to OTAEnd. On failure
 * err_detail (may be NULL) names the step, e.g. "no ESP-Hosted answer" or "OTAWrite refused (-1)". The bus is released on
 * every path. Blocks for up to about a minute.
 */
esp_err_t c6_hosted_install(const c6_hosted_segment_t *segs, size_t count, char *err_detail, size_t err_cap);

#ifdef __cplusplus
}
#endif
