#include "tsens_conv.h"
#include "unity.h"

TEST_CASE("On a raw reading in the -10..80 range (offset 0), no calibration: the HAL's whole degrees", "[tsens]")
{
    float c = 0.0f;

    // (4386 * 140 - 205200) / 10000 = 40.884 -> 40 (integer division, as ESP-IDF's HAL)
    TEST_ASSERT_TRUE(diag_tsens_celsius(140, 0, 0, &c));
    TEST_ASSERT_EQUAL_FLOAT(40.0f, c);
}

TEST_CASE("On a chip with an eFuse calibration: the calibration in tenths is subtracted", "[tsens]")
{
    float c = 0.0f;

    TEST_ASSERT_TRUE(diag_tsens_celsius(140, 0, -5, &c)); // delta -0.5: 40 + 0.5
    TEST_ASSERT_EQUAL_FLOAT(40.5f, c);
    TEST_ASSERT_TRUE(diag_tsens_celsius(140, 0, 12, &c)); // delta +1.2: 40 - 1.2
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 38.8f, c);
}

TEST_CASE("On another range's DAC offset: the offset term applies", "[tsens]")
{
    float c = 0.0f;

    // (4386 * 140 - 278800 * -1 - 205200) / 10000 = 68.764 -> 68
    TEST_ASSERT_TRUE(diag_tsens_celsius(140, -1, 0, &c));
    TEST_ASSERT_EQUAL_FLOAT(68.0f, c);
}

TEST_CASE("On a raw value beyond the sensor's limits: false, the value still set", "[tsens]")
{
    float c = 0.0f;

    // (4386 * 400 - 205200) / 10000 = 154.92 -> 154: above the 125 C measuring limit
    TEST_ASSERT_FALSE(diag_tsens_celsius(400, 0, 0, &c));
    TEST_ASSERT_EQUAL_FLOAT(154.0f, c);
    // raw 0 is -20.52 -> -20: inside the limits
    TEST_ASSERT_TRUE(diag_tsens_celsius(0, 0, 0, &c));
    TEST_ASSERT_EQUAL_FLOAT(-20.0f, c);
}
