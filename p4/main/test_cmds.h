#pragma once

#include "esp_err.h"

/**
 * Register the test-only commands on the console (OTBR_TEST_BUILD only; console_start calls it before the REPL starts) and
 * decide the rollback state of this boot.
 */
esp_err_t test_cmds_register(void);
