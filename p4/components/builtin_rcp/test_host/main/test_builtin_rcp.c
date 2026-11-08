// The built-in RCP's pure decisions: whether the C6 is listed (and the fault kind when not), what a failed ROM
// recovery means, the status's recovery flag and installed version.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "builtin_rcp_core.h"
#include "unity.h"
#include "unity_test_runner.h"

/* ---- builtin_rcp_present (was radio_mux_c6_present) ---- */

TEST_CASE("On no probe yet: present, reason empty", "[builtin_rcp]")
{
    const rcp_sync_probe_result_t p = {0};
    char why[64] = "x";

    TEST_ASSERT_TRUE(builtin_rcp_present(&p, NULL, why, sizeof(why)));
    TEST_ASSERT_EQUAL_STRING("", why);
}

TEST_CASE("On an RCP installed: present", "[builtin_rcp]")
{
    const rcp_sync_probe_result_t p = {.updater_reachable = true, .installed_valid = true};

    TEST_ASSERT_TRUE(builtin_rcp_present(&p, NULL, NULL, 0));
}

TEST_CASE("On a silent updater without a failed recovery: present (the installed firmware is tried)", "[builtin_rcp]")
{
    const rcp_sync_probe_result_t p = {.updater_reachable = false};
    char why[64];

    TEST_ASSERT_TRUE(builtin_rcp_present(&p, NULL, why, sizeof(why)));
    TEST_ASSERT_EQUAL_STRING("", why);
}

TEST_CASE("On the factory ESP-Hosted firmware: unavailable, the reason names it", "[builtin_rcp]")
{
    const rcp_sync_probe_result_t p = {.esp_hosted = true};
    char why[64];

    TEST_ASSERT_FALSE(builtin_rcp_present(&p, NULL, why, sizeof(why)));
    TEST_ASSERT_EQUAL_STRING("the C6 runs its factory ESP-Hosted firmware", why);
}

TEST_CASE("On a failed ROM recovery with a silent updater: unavailable, the reason names it", "[builtin_rcp]")
{
    const rcp_sync_probe_result_t p = {.rom_recovery_failed = true};
    char why[64];

    TEST_ASSERT_FALSE(builtin_rcp_present(&p, NULL, why, sizeof(why)));
    TEST_ASSERT_EQUAL_STRING("the ROM recovery of the C6 failed", why);
}

TEST_CASE("On a failed ROM recovery with the updater answering and an RCP installed: present", "[builtin_rcp]")
{
    const rcp_sync_probe_result_t p = {.rom_recovery_failed = true, .updater_reachable = true, .installed_valid = true};

    TEST_ASSERT_TRUE(builtin_rcp_present(&p, NULL, NULL, 0));
}

TEST_CASE("On the updater answering with no RCP installed: unavailable, the reason names it", "[builtin_rcp]")
{
    const rcp_sync_probe_result_t p = {.updater_reachable = true, .installed_valid = false};
    char why[64];

    TEST_ASSERT_FALSE(builtin_rcp_present(&p, NULL, why, sizeof(why)));
    TEST_ASSERT_EQUAL_STRING("no RCP firmware on the C6", why);
}

TEST_CASE("On no RCP firmware, the ESP-Hosted image or a failed ROM recovery: a firmware fault with today's reason",
          "[builtin_rcp]")
{
    const rcp_sync_probe_result_t faults[] = {
        {.updater_reachable = true, .installed_valid = false},
        {.esp_hosted = true},
        {.rom_recovery_failed = true},
    };
    const rcp_sync_probe_result_t fine = {.updater_reachable = true, .installed_valid = true};
    radio_mux_fault_kind_t kind = RADIO_MUX_FAULT_LINK;
    char why[64];

    for (size_t i = 0; i < sizeof(faults) / sizeof(faults[0]); i++) {
        kind = RADIO_MUX_FAULT_NONE;
        TEST_ASSERT_FALSE(builtin_rcp_present(&faults[i], &kind, why, sizeof(why)));
        TEST_ASSERT_EQUAL(RADIO_MUX_FAULT_FIRMWARE, kind);
        TEST_ASSERT_NOT_EQUAL(0, strlen(why));
    }
    TEST_ASSERT_TRUE(builtin_rcp_present(&fine, &kind, why, sizeof(why)));
    TEST_ASSERT_EQUAL(RADIO_MUX_FAULT_NONE, kind);
}

/* ---- builtin_rcp_rom_failure_is_fault (was mode_fsm_rom_failure_is_fault) ---- */

TEST_CASE("On a failed ROM recovery after a silent updater: the boot enters RADIO_FAULT", "[builtin_rcp]")
{
    rcp_sync_probe_result_t probe = {.rom_recovery_failed = true, .updater_reachable = false};

    TEST_ASSERT_TRUE(builtin_rcp_rom_failure_is_fault(&probe));
}

TEST_CASE("On a failed ROM recovery after a version mismatch (updater reachable): the boot goes on with the installed "
          "RCP", "[builtin_rcp]")
{
    rcp_sync_probe_result_t probe = {.rom_recovery_failed = true, .updater_reachable = true, .installed_valid = true};

    TEST_ASSERT_FALSE(builtin_rcp_rom_failure_is_fault(&probe));
}

TEST_CASE("On a probe without a failed recovery: no fault from the recovery", "[builtin_rcp]")
{
    rcp_sync_probe_result_t probe = {.updater_reachable = false};

    TEST_ASSERT_FALSE(builtin_rcp_rom_failure_is_fault(&probe));
}

/* ---- builtin_rcp_next_recovery_failed (was mode_fsm_next_recovery_failed) ---- */

TEST_CASE("On an automatic recovery failing and then a manual one succeeding: the failed flag is cleared",
          "[builtin_rcp]")
{
    bool flag = builtin_rcp_next_recovery_failed(false, true, false);

    TEST_ASSERT_TRUE(flag);
    flag = builtin_rcp_next_recovery_failed(flag, true, true);
    TEST_ASSERT_FALSE(flag);
}

TEST_CASE("On a manual recovery failing: the failed flag is set", "[builtin_rcp]")
{
    TEST_ASSERT_TRUE(builtin_rcp_next_recovery_failed(false, true, false));
}

TEST_CASE("On a probe-time recovery succeeding after an earlier failure: the failed flag is cleared", "[builtin_rcp]")
{
    TEST_ASSERT_FALSE(builtin_rcp_next_recovery_failed(true, true, true));
}

TEST_CASE("On no recovery attempt: the failed flag is unchanged", "[builtin_rcp]")
{
    TEST_ASSERT_TRUE(builtin_rcp_next_recovery_failed(true, false, false));
    TEST_ASSERT_FALSE(builtin_rcp_next_recovery_failed(false, false, false));
}

/* ---- builtin_rcp_installed_after_push (was mode_fsm_installed_after_push) ---- */

TEST_CASE("On a push whose version was read back: the new version is known", "[builtin_rcp]")
{
    bool known = false;
    char installed[32] = "1.0.0";

    builtin_rcp_installed_after_push(true, "2.0.0", &known, installed, sizeof(installed));
    TEST_ASSERT_TRUE(known);
    TEST_ASSERT_EQUAL_STRING("2.0.0", installed);
}

TEST_CASE("On a push whose version could not be read back: unknown and the old version is gone", "[builtin_rcp]")
{
    bool known = true;
    char installed[32] = "1.0.0";

    builtin_rcp_installed_after_push(false, "", &known, installed, sizeof(installed));
    TEST_ASSERT_FALSE(known);
    TEST_ASSERT_EQUAL_STRING("", installed);
}

void app_main(void)
{
    UNITY_BEGIN();
    unity_run_all_tests();
    int failures = (int)UNITY_END();
    exit(failures == 0 ? 0 : 1);
}
