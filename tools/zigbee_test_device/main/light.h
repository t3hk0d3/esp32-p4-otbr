#pragma once

#include <stdbool.h>
#include "esp_err.h"

/** Start the WS2812B on GPIO8 (the devkit's RGB LED), dark, and print `light: off`. */
esp_err_t light_init(void);

/** Switch the light (on = white, off = dark) and print `light: on` / `light: off` when the state changed. */
esp_err_t light_set(bool on);

bool light_is_on(void);
