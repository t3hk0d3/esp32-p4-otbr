#include <stdlib.h>
#include "unity.h"
#include "unity_test_runner.h"

void app_main(void)
{
    UNITY_BEGIN();
    unity_run_all_tests();
    int failures = (int)UNITY_END();
    exit(failures == 0 ? 0 : 1);
}
