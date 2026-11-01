#pragma once

#include "esp_err.h"

/**
 * Start the console REPL on the console UART ("p4> "). Registers `net show` / `net reset` (netcfg) in every build and, in test
 * builds (OTBR_TEST_BUILD), the test-only commands.
 */
esp_err_t console_start(void);
