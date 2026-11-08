#include "tsens_conv.h"

bool diag_tsens_celsius(uint32_t raw, int range_offset, int delta_t_tenths, float *out)
{
    const int degree = (DIAG_TSENS_ADC_FACTOR_INT * (int)raw - DIAG_TSENS_DAC_FACTOR_INT * range_offset -
                        DIAG_TSENS_OFFSET_FACTOR_INT) / DIAG_TSENS_DENOMINATOR;

    *out = (float)degree - (float)delta_t_tenths / 10.0f;
    return *out >= DIAG_TSENS_MEASURE_MIN_C && *out <= DIAG_TSENS_MEASURE_MAX_C;
}
