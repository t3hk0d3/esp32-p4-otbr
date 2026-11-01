#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialise NVS. Erases and re-initialises it only for ESP_ERR_NVS_NEW_VERSION_FOUND; any other
 * error, including a full or corrupt partition, is returned untouched. Device settings use the
 * namespace "otbr_dev".
 */
esp_err_t storage_init(void);

/** Index (0 or 1) of the running app slot ota_N, or -1 if it cannot be determined. */
int storage_running_slot(void);

/**
 * Mount web_<running slot> at /web (SPIFFS, read-only use). There are no rcp partitions: the RCP firmware lives
 * in the C6 (R14). A web slot that does not mount is logged and returned without touching the data. The
 * inactive web slot is never mounted.
 */
esp_err_t storage_mount(void);

#ifdef __cplusplus
}
#endif
