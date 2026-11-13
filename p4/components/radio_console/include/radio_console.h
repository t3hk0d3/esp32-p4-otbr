// Console command `radio-fw` (docs/reference/radio-console.md): the radio's state, and one C6 image taken over the
// UART0 console and passed to builtin_rcp_push_firmware. The browser flasher uses it on a board it has just flashed.
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Registers `radio-fw` with esp_console. After esp_console_new_repl_uart (the command reads that UART's driver). */
esp_err_t radio_console_register(void);

#ifdef __cplusplus
}
#endif
