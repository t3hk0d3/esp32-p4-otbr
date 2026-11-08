#pragma once

#include <stdbool.h>
#include <stdint.h>

// The chip temperature sensor's conversion, as ESP-IDF v6.1 does it (temperature_sensor_hal.c and the driver's
// parse_temp_sensor_raw_value), for the one fixed range the sampler uses. Pure: host-tested.

// ESP-IDF v6.1's ESP32-P4 factors (esp_hal_ana_conv/esp32p4/include/hal/temperature_sensor_ll.h); sampler.c checks
// them against the HAL at build time.
#define DIAG_TSENS_ADC_FACTOR_INT 4386
#define DIAG_TSENS_DAC_FACTOR_INT 278800
#define DIAG_TSENS_OFFSET_FACTOR_INT 205200
#define DIAG_TSENS_DENOMINATOR 10000
// The sensor's measuring limits (TEMPERATURE_SENSOR_LL_MEASURE_MIN/MAX): outside them a reading is not a temperature.
#define DIAG_TSENS_MEASURE_MIN_C (-40)
#define DIAG_TSENS_MEASURE_MAX_C 125

/**
 * Degrees Celsius for a raw sensor output `raw` read in the range whose DAC offset is `range_offset`, with the chip's
 * eFuse calibration `delta_t_tenths` (tenths of a degree): the HAL's whole degrees (integer division, as upstream)
 * minus the calibration. False when the result is outside the sensor's measuring limits (`*out` is still set).
 */
bool diag_tsens_celsius(uint32_t raw, int range_offset, int delta_t_tenths, float *out);
