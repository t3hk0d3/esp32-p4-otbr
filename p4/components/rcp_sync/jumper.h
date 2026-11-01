#pragma once

// The maintenance jumper's UART side (spec 5.10): passive detection of the C6 ROM boot banner and capture of the C6
// console. Internal to rcp_sync; link_spi.c starts it, rom_flash.c pauses it.

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define JUMPER_UART_BAUD 115200
#define JUMPER_UART_PORT 1 /* UART_NUM_1: the log receiver and, while it is paused, the flasher */

/**
 * Install the UART receiver on JUMPER_RX (receive only: JUMPER_TX and JUMPER_BOOT are never configured here) and start
 * the capture task on core 1. Idempotent; returns once the receiver listens, so a C6 reset right after is seen.
 */
esp_err_t jumper_start(void);

/**
 * Hand the UART over to the flasher: the capture task releases the driver and waits. No-op when never started.
 * ESP_ERR_TIMEOUT if the task does not answer (then the UART is not free; jumper_log_resume is still due).
 */
esp_err_t jumper_log_pause(void);

/** Take the UART back after jumper_log_pause. */
void jumper_log_resume(void);

#ifdef __cplusplus
}
#endif
