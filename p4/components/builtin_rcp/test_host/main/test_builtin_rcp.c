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

/* ---- builtin_rcp_job_result (was mode_fsm_radio_job_result) ---- */

TEST_CASE("On a push or a recovery that failed: the job result is its error, whatever the mode did", "[builtin_rcp]")
{
    TEST_ASSERT_EQUAL(ESP_FAIL, builtin_rcp_job_result(true, ESP_FAIL, RADIO_MUX_SETTLE_UP));
    TEST_ASSERT_EQUAL(ESP_FAIL, builtin_rcp_job_result(true, ESP_FAIL, RADIO_MUX_SETTLE_DOWN));
    TEST_ASSERT_EQUAL(ESP_FAIL, builtin_rcp_job_result(false, ESP_FAIL, RADIO_MUX_SETTLE_UP));
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, builtin_rcp_job_result(false, ESP_ERR_TIMEOUT, RADIO_MUX_SETTLE_DOWN));
}

TEST_CASE("On a base-only push or a ROM recovery that worked: the job result is ESP_OK whatever the mode did",
          "[builtin_rcp]")
{
    TEST_ASSERT_EQUAL(ESP_OK, builtin_rcp_job_result(true, ESP_OK, RADIO_MUX_SETTLE_DOWN));
    TEST_ASSERT_EQUAL(ESP_OK, builtin_rcp_job_result(true, ESP_OK, RADIO_MUX_SETTLE_WAIT));
    TEST_ASSERT_EQUAL(ESP_OK, builtin_rcp_job_result(true, ESP_OK, RADIO_MUX_SETTLE_UP));
}

TEST_CASE("On a push that worked and a mode that is up, or no mode: the job result is ESP_OK", "[builtin_rcp]")
{
    TEST_ASSERT_EQUAL(ESP_OK, builtin_rcp_job_result(false, ESP_OK, RADIO_MUX_SETTLE_UP));
    TEST_ASSERT_EQUAL(ESP_OK, builtin_rcp_job_result(false, ESP_OK, RADIO_MUX_SETTLE_NONE));
}

TEST_CASE("On a push that worked and a mode that is down or still waiting: the job result is RCP_SYNC_ERR_RESTART",
          "[builtin_rcp]")
{
    TEST_ASSERT_EQUAL(RCP_SYNC_ERR_RESTART, builtin_rcp_job_result(false, ESP_OK, RADIO_MUX_SETTLE_DOWN));
    TEST_ASSERT_EQUAL(RCP_SYNC_ERR_RESTART, builtin_rcp_job_result(false, ESP_OK, RADIO_MUX_SETTLE_WAIT));
}

/* ---- builtin_rcp_restart_detail ---- */

static radio_mux_maint_t outcome(radio_mux_mode_t mode, radio_mux_settle_t settle, const char *state,
                                 const char *reason)
{
    radio_mux_maint_t m = {.ran = true, .mode = mode, .settle = settle};

    snprintf(m.mode_state, sizeof(m.mode_state), "%s", state);
    snprintf(m.mode_reason, sizeof(m.mode_reason), "%s", reason);
    return m;
}

TEST_CASE("On Thread down with a reason: the detail names OpenThread and the reason", "[builtin_rcp]")
{
    const radio_mux_maint_t m = outcome(RADIO_MUX_MODE_OTBR, RADIO_MUX_SETTLE_DOWN, "fault", "no rcp firmware");
    char out[128];

    builtin_rcp_restart_detail(out, sizeof(out), &m);
    TEST_ASSERT_EQUAL_STRING("the image is installed, but OpenThread did not start on it: no rcp firmware", out);
}

TEST_CASE("On Zigbee down with a reason: the detail names Zigbee and the reason", "[builtin_rcp]")
{
    const radio_mux_maint_t m = outcome(RADIO_MUX_MODE_ZIGGURAT, RADIO_MUX_SETTLE_DOWN, "incompatible",
                                        "firmware too old");
    char out[128];

    builtin_rcp_restart_detail(out, sizeof(out), &m);
    TEST_ASSERT_EQUAL_STRING("the image is installed, but Zigbee did not start on it: firmware too old", out);
}

TEST_CASE("On the proxy down with no reason: the detail names rcp-proxy and its state", "[builtin_rcp]")
{
    const radio_mux_maint_t m = outcome(RADIO_MUX_MODE_RCP_PROXY, RADIO_MUX_SETTLE_DOWN, "no_radio", "");
    char out[128];

    builtin_rcp_restart_detail(out, sizeof(out), &m);
    TEST_ASSERT_EQUAL_STRING("the image is installed, but rcp-proxy did not start on it: no_radio", out);
}

TEST_CASE("On a mode still waiting after the timeout: the detail says it is still starting after 180 s",
          "[builtin_rcp]")
{
    const radio_mux_maint_t m = outcome(RADIO_MUX_MODE_OTBR, RADIO_MUX_SETTLE_WAIT, "starting", "");
    char out[128];

    builtin_rcp_restart_detail(out, sizeof(out), &m);
    TEST_ASSERT_EQUAL_STRING("the image is installed, but OpenThread did not start on it: still starting after 180 s",
                             out);
}

TEST_CASE("On a buffer too small for the detail: truncated and terminated", "[builtin_rcp]")
{
    const radio_mux_maint_t m = outcome(RADIO_MUX_MODE_OTBR, RADIO_MUX_SETTLE_DOWN, "fault", "no rcp firmware");
    char out[16];

    memset(out, 'x', sizeof(out));
    builtin_rcp_restart_detail(out, sizeof(out), &m);
    TEST_ASSERT_EQUAL(15, strlen(out));
    TEST_ASSERT_EQUAL_STRING("the image is in", out);
}

TEST_CASE("On the abandoned-start reason: the documented detail fits BUILTIN_RCP_DETAIL_MAX whole", "[builtin_rcp]")
{
    const radio_mux_maint_t m = outcome(RADIO_MUX_MODE_OTBR, RADIO_MUX_SETTLE_DOWN, "fault",
                                        "an earlier OpenThread start was abandoned: reboot");
    char out[BUILTIN_RCP_DETAIL_MAX];

    builtin_rcp_restart_detail(out, sizeof(out), &m);
    TEST_ASSERT_EQUAL_STRING("the image is installed, but OpenThread did not start on it: an earlier OpenThread start "
                             "was abandoned: reboot", out);
}

TEST_CASE("On the longest reason a mode can report: the detail fits BUILTIN_RCP_DETAIL_MAX whole", "[builtin_rcp]")
{
    char reason[RADIO_MUX_REASON_MAX];
    char out[BUILTIN_RCP_DETAIL_MAX];

    memset(reason, 'x', sizeof(reason) - 1);
    reason[sizeof(reason) - 1] = '\0';
    const radio_mux_maint_t m = outcome(RADIO_MUX_MODE_OTBR, RADIO_MUX_SETTLE_DOWN, "fault", reason);

    builtin_rcp_restart_detail(out, sizeof(out), &m);
    TEST_ASSERT_EQUAL(strlen("the image is installed, but OpenThread did not start on it: ") + strlen(reason),
                      strlen(out));
}
