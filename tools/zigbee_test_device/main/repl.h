#pragma once

#include "esp_err.h"

/** Start the console on UART0 with the commands `state` and `factory_reset`. */
esp_err_t repl_start(void);
