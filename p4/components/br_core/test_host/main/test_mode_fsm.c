#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "unity.h"
#include "unity_test_runner.h"
#include "mode_fsm.h"

#define MINUTE_US (60LL * 1000 * 1000)
#define LOG_CAP 16

/* ---- fake ops: record the call order, fail on demand ---- */

typedef struct {
    const char *calls[LOG_CAP];
    int count;
    esp_err_t fail[16]; /* per op name index, see OP_* */
    int64_t now;
    uint8_t raw_channel;
    bool push_restarted; /* what the push op reports: OpenThread runs again */
    br_core_mode_t mode_in_job; /* the mode the push or ROM recovery op saw */
    otLinkRawReceiveDone raw_cb;
    void (*deferred_fn)(void *ctx); /* what the defer op was given, run by run_deferred() */
    void *deferred_ctx;
} fake_t;

enum { OP_THREAD_STOP, OP_NETIF_DOWN, OP_RAW_ENABLE, OP_RAW_DISABLE, OP_RESTORE_CHANNEL, OP_NETIF_UP, OP_THREAD_START,
       OP_RESET_RCP, OP_PUSH, OP_RAW_ABORT, OP_QUIET, OP_DEFER, OP_ROM_RECOVER, OP_COUNT
     };

static fake_t fake;
static mode_fsm_t fsm;

static esp_err_t record(int op, const char *name)
{
    if (fake.count < LOG_CAP) {
        fake.calls[fake.count++] = name;
    }
    return fake.fail[op];
}

static esp_err_t f_thread_stop(void)
{
    return record(OP_THREAD_STOP, "thread_stop");
}
static esp_err_t f_netif_down(void)
{
    return record(OP_NETIF_DOWN, "netif_down");
}
static esp_err_t f_raw_enable(uint8_t channel, otLinkRawReceiveDone cb)
{
    fake.raw_channel = channel;
    fake.raw_cb = cb;
    return record(OP_RAW_ENABLE, "raw_enable");
}
static esp_err_t f_raw_disable(void)
{
    return record(OP_RAW_DISABLE, "raw_disable");
}
static esp_err_t f_raw_abort(void)
{
    return record(OP_RAW_ABORT, "raw_abort");
}
static esp_err_t f_quiet_radio(void)
{
    return record(OP_QUIET, "quiet_radio");
}
static esp_err_t f_defer(void (*fn)(void *ctx), void *ctx)
{
    fake.deferred_fn = fn;
    fake.deferred_ctx = ctx;
    return record(OP_DEFER, "defer");
}
static void run_deferred(void)
{
    TEST_ASSERT_NOT_NULL(fake.deferred_fn);
    fake.deferred_fn(fake.deferred_ctx);
}
static esp_err_t f_restore_channel(void)
{
    return record(OP_RESTORE_CHANNEL, "restore_channel");
}
static esp_err_t f_netif_up(void)
{
    return record(OP_NETIF_UP, "netif_up");
}
static esp_err_t f_thread_start(void)
{
    return record(OP_THREAD_START, "thread_start");
}
static esp_err_t f_reset_rcp(void)
{
    return record(OP_RESET_RCP, "reset_rcp");
}
static esp_err_t f_push(const rcp_fw_source_t *source, char *err_detail, size_t err_cap, bool *restarted)
{
    *restarted = fake.push_restarted;
    fake.mode_in_job = mode_fsm_get_mode(&fsm);
    return record(OP_PUSH, "push_firmware");
}
static esp_err_t f_rom_recover(char *err_detail, size_t err_cap, bool *restarted)
{
    *restarted = fake.push_restarted;
    return record(OP_ROM_RECOVER, "rom_recover");
}
static void f_reboot(void)
{
    if (fake.count < LOG_CAP) {
        fake.calls[fake.count++] = "reboot";
    }
}
static int64_t f_now(void)
{
    return fake.now;
}

static const mode_fsm_ops_t ops = {
    .thread_stop = f_thread_stop,
    .netif_down = f_netif_down,
    .raw_enable = f_raw_enable,
    .raw_disable = f_raw_disable,
    .raw_abort = f_raw_abort,
    .quiet_radio = f_quiet_radio,
    .defer = f_defer,
    .restore_channel = f_restore_channel,
    .netif_up = f_netif_up,
    .thread_start = f_thread_start,
    .reset_rcp = f_reset_rcp,
    .push_firmware = f_push,
    .rom_recover = f_rom_recover,
    .reboot = f_reboot,
    .now_us = f_now,
};

static void rx_done(otInstance *instance, otRadioFrame *frame, otError error)
{
}

static int end_cb_calls;
static br_core_mode_t end_cb_mode; /* the mode the callback saw */
static void end_cb(void)
{
    end_cb_calls++;
    end_cb_mode = mode_fsm_get_mode(&fsm);
}

static void setup(br_core_mode_t mode)
{
    memset(&fake, 0, sizeof(fake));
    end_cb_calls = 0;
    mode_fsm_init(&fsm, &ops);
    atomic_store(&fsm.mode, mode);
    fsm.thread_stopped_by_capture = mode == BR_MODE_SNIFFER; /* a capture stopped Thread */
}

static int index_of(const char *name)
{
    for (int i = 0; i < fake.count; i++) {
        if (strcmp(fake.calls[i], name) == 0) {
            return i;
        }
    }
    return -1;
}

static int count_of(const char *name)
{
    int n = 0;
    for (int i = 0; i < fake.count; i++) {
        n += strcmp(fake.calls[i], name) == 0;
    }
    return n;
}

static void assert_order(const char *first, const char *second)
{
    TEST_ASSERT_TRUE_MESSAGE(index_of(first) >= 0, first);
    TEST_ASSERT_TRUE_MESSAGE(index_of(second) >= 0, second);
    TEST_ASSERT_TRUE_MESSAGE(index_of(first) < index_of(second), first);
}

/* ---- enter sniffer ---- */

TEST_CASE("On BR_RUNNING and valid channel: stops Thread, then takes the OpenThread netif down", "[mode_fsm]")
{
    setup(BR_MODE_BR_RUNNING);
    TEST_ASSERT_EQUAL(ESP_OK, mode_fsm_enter_sniffer(&fsm, 15, rx_done));
    assert_order("thread_stop", "netif_down");
    assert_order("netif_down", "raw_enable");
}

TEST_CASE("On BR_RUNNING and valid channel: enables the raw link on the channel with the callback", "[mode_fsm]")
{
    setup(BR_MODE_BR_RUNNING);
    TEST_ASSERT_EQUAL(ESP_OK, mode_fsm_enter_sniffer(&fsm, 15, rx_done));
    TEST_ASSERT_EQUAL(1, count_of("raw_enable"));
    TEST_ASSERT_EQUAL(15, fake.raw_channel);
    TEST_ASSERT_TRUE(fake.raw_cb == rx_done);
}

TEST_CASE("On BR_RUNNING and valid channel: enters SNIFFER", "[mode_fsm]")
{
    setup(BR_MODE_BR_RUNNING);
    TEST_ASSERT_EQUAL(ESP_OK, mode_fsm_enter_sniffer(&fsm, 11, rx_done));
    TEST_ASSERT_EQUAL(BR_MODE_SNIFFER, mode_fsm_get_mode(&fsm));
}

TEST_CASE("On mode other than BR_RUNNING: returns ESP_ERR_INVALID_STATE", "[mode_fsm]")
{
    const br_core_mode_t modes[] = {BR_MODE_STARTING, BR_MODE_SNIFFER, BR_MODE_RADIO_FAULT};
    for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
        setup(modes[i]);
        TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, mode_fsm_enter_sniffer(&fsm, 15, rx_done));
        TEST_ASSERT_EQUAL(0, fake.count);
        TEST_ASSERT_EQUAL(modes[i], mode_fsm_get_mode(&fsm));
    }
}

TEST_CASE("On channel outside 11..26: returns ESP_ERR_INVALID_ARG and stays BR_RUNNING", "[mode_fsm]")
{
    const uint8_t channels[] = {0, 10, 27, 255};
    for (size_t i = 0; i < sizeof(channels); i++) {
        setup(BR_MODE_BR_RUNNING);
        TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, mode_fsm_enter_sniffer(&fsm, channels[i], rx_done));
        TEST_ASSERT_EQUAL(BR_MODE_BR_RUNNING, mode_fsm_get_mode(&fsm));
        TEST_ASSERT_EQUAL(0, fake.count); /* Thread was not touched */
    }
}

TEST_CASE("On raw_enable failure: restores BR (netif up, thread start) and returns the error", "[mode_fsm]")
{
    setup(BR_MODE_BR_RUNNING);
    fake.fail[OP_RAW_ENABLE] = ESP_FAIL;
    TEST_ASSERT_EQUAL(ESP_FAIL, mode_fsm_enter_sniffer(&fsm, 15, rx_done));
    assert_order("raw_enable", "netif_up");
    assert_order("netif_up", "thread_start");
    TEST_ASSERT_EQUAL(BR_MODE_BR_RUNNING, mode_fsm_get_mode(&fsm));
    TEST_ASSERT_EQUAL(0, count_of("reboot"));
}

TEST_CASE("On NULL receive callback: returns ESP_ERR_INVALID_ARG, touches nothing and stays BR_RUNNING", "[mode_fsm]")
{
    setup(BR_MODE_BR_RUNNING);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, mode_fsm_enter_sniffer(&fsm, 15, NULL));
    TEST_ASSERT_EQUAL(0, fake.count);
    TEST_ASSERT_EQUAL(BR_MODE_BR_RUNNING, mode_fsm_get_mode(&fsm));
}

TEST_CASE("On thread_stop failure: skips the netif and the raw link, restores BR and returns the error", "[mode_fsm]")
{
    setup(BR_MODE_BR_RUNNING);
    fake.fail[OP_THREAD_STOP] = ESP_FAIL;
    TEST_ASSERT_EQUAL(ESP_FAIL, mode_fsm_enter_sniffer(&fsm, 15, rx_done));
    TEST_ASSERT_EQUAL(0, count_of("netif_down"));
    TEST_ASSERT_EQUAL(0, count_of("raw_enable"));
    assert_order("netif_up", "thread_start");
    TEST_ASSERT_EQUAL(BR_MODE_BR_RUNNING, mode_fsm_get_mode(&fsm));
    TEST_ASSERT_EQUAL(0, count_of("reboot"));
}

TEST_CASE("On netif_down failure: skips the raw link, restores BR and returns the error", "[mode_fsm]")
{
    setup(BR_MODE_BR_RUNNING);
    fake.fail[OP_NETIF_DOWN] = ESP_FAIL;
    TEST_ASSERT_EQUAL(ESP_FAIL, mode_fsm_enter_sniffer(&fsm, 15, rx_done));
    TEST_ASSERT_EQUAL(0, count_of("raw_enable"));
    assert_order("netif_up", "thread_start");
    TEST_ASSERT_EQUAL(BR_MODE_BR_RUNNING, mode_fsm_get_mode(&fsm));
    TEST_ASSERT_EQUAL(0, count_of("reboot"));
}

TEST_CASE("On netif_up failure while restoring after a failed enter: reboots", "[mode_fsm]")
{
    setup(BR_MODE_BR_RUNNING);
    fake.fail[OP_RAW_ENABLE] = ESP_FAIL;
    fake.fail[OP_NETIF_UP] = ESP_FAIL;
    TEST_ASSERT_EQUAL(ESP_FAIL, mode_fsm_enter_sniffer(&fsm, 15, rx_done));
    TEST_ASSERT_EQUAL(1, count_of("reboot"));
}

/* ---- leave sniffer ---- */

TEST_CASE("On stop request: restores the dataset channel and restarts Thread", "[mode_fsm]")
{
    setup(BR_MODE_SNIFFER);
    TEST_ASSERT_EQUAL(ESP_OK, mode_fsm_leave_sniffer(&fsm));
    assert_order("raw_disable", "restore_channel");
    assert_order("restore_channel", "netif_up");
    assert_order("netif_up", "thread_start");
}

TEST_CASE("On stop request: enters BR_RUNNING", "[mode_fsm]")
{
    setup(BR_MODE_SNIFFER);
    TEST_ASSERT_EQUAL(ESP_OK, mode_fsm_leave_sniffer(&fsm));
    TEST_ASSERT_EQUAL(BR_MODE_BR_RUNNING, mode_fsm_get_mode(&fsm));
    TEST_ASSERT_EQUAL(0, count_of("reboot"));
}

TEST_CASE("On restore failure (raw_disable, restore_channel, netif_up or thread_start failing): reboots", "[mode_fsm]")
{
    const int failing[] = {OP_RAW_DISABLE, OP_RESTORE_CHANNEL, OP_NETIF_UP, OP_THREAD_START};
    for (size_t i = 0; i < sizeof(failing) / sizeof(failing[0]); i++) {
        setup(BR_MODE_SNIFFER);
        fake.fail[failing[i]] = ESP_FAIL;
        TEST_ASSERT_EQUAL(ESP_FAIL, mode_fsm_leave_sniffer(&fsm));
        TEST_ASSERT_EQUAL(1, count_of("reboot"));
        TEST_ASSERT_NOT_EQUAL(BR_MODE_BR_RUNNING, mode_fsm_get_mode(&fsm));
    }
}

TEST_CASE("On a mode other than SNIFFER: leave returns ESP_ERR_INVALID_STATE and touches nothing", "[mode_fsm]")
{
    setup(BR_MODE_BR_RUNNING);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, mode_fsm_leave_sniffer(&fsm));
    TEST_ASSERT_EQUAL(0, fake.count);
}

/* ---- radio fault recovery ---- */

TEST_CASE("On RCP failure callback: resets the C6 and restarts OpenThread", "[mode_fsm]")
{
    setup(BR_MODE_BR_RUNNING);
    mode_fsm_on_rcp_failure(&fsm);
    TEST_ASSERT_EQUAL(1, count_of("reset_rcp"));
}

TEST_CASE("On RCP failure callback: enters BR_RUNNING on success", "[mode_fsm]")
{
    setup(BR_MODE_BR_RUNNING);
    mode_fsm_on_rcp_failure(&fsm);
    TEST_ASSERT_EQUAL(BR_MODE_BR_RUNNING, mode_fsm_get_mode(&fsm));
}

TEST_CASE("On RCP failure callback: never calls push_firmware", "[mode_fsm]")
{
    setup(BR_MODE_BR_RUNNING);
    mode_fsm_on_rcp_failure(&fsm);
    fake.fail[OP_RESET_RCP] = ESP_FAIL;
    mode_fsm_on_rcp_failure(&fsm);
    TEST_ASSERT_EQUAL(0, count_of("push_firmware"));
}

TEST_CASE("On RCP failure when the reset fails: stays in RADIO_FAULT", "[mode_fsm]")
{
    setup(BR_MODE_BR_RUNNING);
    fake.fail[OP_RESET_RCP] = ESP_FAIL;
    mode_fsm_on_rcp_failure(&fsm);
    TEST_ASSERT_EQUAL(BR_MODE_RADIO_FAULT, mode_fsm_get_mode(&fsm));
    TEST_ASSERT_EQUAL(1, count_of("reset_rcp"));
}

TEST_CASE("On 4th failure within 10 minutes: stays in RADIO_FAULT without automatic recovery", "[mode_fsm]")
{
    setup(BR_MODE_BR_RUNNING);
    for (int i = 0; i < 3; i++) {
        fake.now = i * MINUTE_US;
        mode_fsm_on_rcp_failure(&fsm);
        TEST_ASSERT_EQUAL(BR_MODE_BR_RUNNING, mode_fsm_get_mode(&fsm));
    }
    TEST_ASSERT_EQUAL(3, count_of("reset_rcp"));
    fake.now = 9 * MINUTE_US;
    mode_fsm_on_rcp_failure(&fsm);
    TEST_ASSERT_EQUAL(BR_MODE_RADIO_FAULT, mode_fsm_get_mode(&fsm));
    TEST_ASSERT_EQUAL(3, count_of("reset_rcp"));
    TEST_ASSERT_EQUAL(0, count_of("push_firmware"));
}

TEST_CASE("On a failure after the window has passed: recovers again", "[mode_fsm]")
{
    setup(BR_MODE_BR_RUNNING);
    for (int i = 0; i < 3; i++) {
        mode_fsm_on_rcp_failure(&fsm); /* all at t = 0 */
    }
    fake.now = 10 * MINUTE_US;
    mode_fsm_on_rcp_failure(&fsm);
    TEST_ASSERT_EQUAL(BR_MODE_BR_RUNNING, mode_fsm_get_mode(&fsm));
    TEST_ASSERT_EQUAL(4, count_of("reset_rcp"));
}

TEST_CASE("On a failure 1 us before the window ends: stays blocked", "[mode_fsm]")
{
    setup(BR_MODE_BR_RUNNING);
    for (int i = 0; i < 3; i++) {
        mode_fsm_on_rcp_failure(&fsm); /* all at t = 0 */
    }
    fake.now = 10 * MINUTE_US - 1;
    mode_fsm_on_rcp_failure(&fsm);
    TEST_ASSERT_EQUAL(BR_MODE_RADIO_FAULT, mode_fsm_get_mode(&fsm));
    TEST_ASSERT_EQUAL(3, count_of("reset_rcp"));
}

TEST_CASE("On RCP failure while in SNIFFER: enters RADIO_FAULT and ends the capture", "[mode_fsm]")
{
    setup(BR_MODE_SNIFFER);
    fake.fail[OP_RESET_RCP] = ESP_FAIL;
    mode_fsm_on_rcp_failure(&fsm);
    TEST_ASSERT_EQUAL(BR_MODE_RADIO_FAULT, mode_fsm_get_mode(&fsm));
    TEST_ASSERT_EQUAL(1, count_of("raw_abort"));
    TEST_ASSERT_EQUAL(0, count_of("raw_disable")); /* no request to the failed RCP */
    assert_order("raw_abort", "reset_rcp");
    TEST_ASSERT_EQUAL(0, count_of("push_firmware"));
}

TEST_CASE("On RCP failure while in SNIFFER and a successful reset: Thread is restored in a deferred step, not in the hook", "[mode_fsm]")
{
    setup(BR_MODE_SNIFFER);
    mode_fsm_on_rcp_failure(&fsm);
    assert_order("reset_rcp", "defer");
    TEST_ASSERT_EQUAL(0, count_of("restore_channel"));
    TEST_ASSERT_EQUAL(0, count_of("thread_start"));
    TEST_ASSERT_EQUAL(BR_MODE_RADIO_FAULT, mode_fsm_get_mode(&fsm));
    run_deferred();
    assert_order("quiet_radio", "restore_channel");
    assert_order("netif_up", "thread_start");
    TEST_ASSERT_EQUAL(BR_MODE_BR_RUNNING, mode_fsm_get_mode(&fsm));
}

TEST_CASE("On the deferred restore after the mode changed meanwhile: touches nothing", "[mode_fsm]")
{
    setup(BR_MODE_SNIFFER);
    mode_fsm_on_rcp_failure(&fsm);
    atomic_store(&fsm.mode, BR_MODE_BR_RUNNING);
    run_deferred();
    TEST_ASSERT_EQUAL(0, count_of("quiet_radio"));
    TEST_ASSERT_EQUAL(0, count_of("thread_start"));
}

TEST_CASE("On a later failure round after a failed capture recovery: Thread is still restored, not left down in BR_RUNNING", "[mode_fsm]")
{
    setup(BR_MODE_BR_RUNNING);
    TEST_ASSERT_EQUAL(ESP_OK, mode_fsm_enter_sniffer(&fsm, 15, rx_done));
    fake.fail[OP_RESET_RCP] = ESP_FAIL;
    mode_fsm_on_rcp_failure(&fsm); /* the capture fails and the reset fails: RADIO_FAULT, Thread stopped */
    TEST_ASSERT_EQUAL(BR_MODE_RADIO_FAULT, mode_fsm_get_mode(&fsm));
    fake.fail[OP_RESET_RCP] = ESP_OK;
    mode_fsm_on_rcp_failure(&fsm); /* next round: the reset works */
    TEST_ASSERT_EQUAL(BR_MODE_RADIO_FAULT, mode_fsm_get_mode(&fsm)); /* not BR_RUNNING yet */
    run_deferred();
    assert_order("netif_up", "thread_start");
    TEST_ASSERT_EQUAL(BR_MODE_BR_RUNNING, mode_fsm_get_mode(&fsm));
}

TEST_CASE("On a failure round without a capture before it: sets BR_RUNNING at once and restores nothing", "[mode_fsm]")
{
    setup(BR_MODE_BR_RUNNING);
    mode_fsm_on_rcp_failure(&fsm);
    TEST_ASSERT_EQUAL(0, count_of("defer"));
    TEST_ASSERT_EQUAL(0, count_of("thread_start"));
    TEST_ASSERT_EQUAL(BR_MODE_BR_RUNNING, mode_fsm_get_mode(&fsm));
}

TEST_CASE("On RCP failure in SNIFFER when scheduling the restore fails: reboots", "[mode_fsm]")
{
    setup(BR_MODE_SNIFFER);
    fake.fail[OP_DEFER] = ESP_FAIL;
    mode_fsm_on_rcp_failure(&fsm);
    TEST_ASSERT_EQUAL(1, count_of("reboot"));
}

TEST_CASE("On the deferred restore failing (thread_start): reboots and stays out of BR_RUNNING", "[mode_fsm]")
{
    setup(BR_MODE_SNIFFER);
    fake.fail[OP_THREAD_START] = ESP_FAIL;
    mode_fsm_on_rcp_failure(&fsm);
    run_deferred();
    TEST_ASSERT_EQUAL(1, count_of("reboot"));
    TEST_ASSERT_EQUAL(BR_MODE_RADIO_FAULT, mode_fsm_get_mode(&fsm));
}

TEST_CASE("On RCP failure while STARTING: only resets the C6, no mode change, no recovery accounting", "[mode_fsm]")
{
    setup(BR_MODE_STARTING);
    for (int i = 0; i < 5; i++) {
        mode_fsm_on_rcp_failure(&fsm);
    }
    TEST_ASSERT_EQUAL(5, count_of("reset_rcp"));
    TEST_ASSERT_EQUAL(BR_MODE_STARTING, mode_fsm_get_mode(&fsm));
    TEST_ASSERT_EQUAL(0, fsm.recovery_count);
}

/* ---- firmware push ---- */

static rcp_fw_source_t push_source;
static char push_detail[32];

TEST_CASE("On push in SNIFFER or STARTING: returns ESP_ERR_INVALID_STATE and starts nothing", "[mode_fsm]")
{
    setup(BR_MODE_SNIFFER);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, mode_fsm_push_firmware(&fsm, &push_source, push_detail, sizeof(push_detail)));
    TEST_ASSERT_EQUAL(0, fake.count);
    TEST_ASSERT_EQUAL(BR_MODE_SNIFFER, mode_fsm_get_mode(&fsm));
    setup(BR_MODE_STARTING);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, mode_fsm_push_firmware(&fsm, &push_source, push_detail, sizeof(push_detail)));
    TEST_ASSERT_EQUAL(0, fake.count);
}

TEST_CASE("On push in RADIO_FAULT with OpenThread running again: enters BR_RUNNING", "[mode_fsm]")
{
    setup(BR_MODE_RADIO_FAULT);
    fake.push_restarted = true;
    TEST_ASSERT_EQUAL(ESP_OK, mode_fsm_push_firmware(&fsm, &push_source, push_detail, sizeof(push_detail)));
    TEST_ASSERT_EQUAL(BR_MODE_BR_RUNNING, mode_fsm_get_mode(&fsm));
    TEST_ASSERT_EQUAL(1, count_of("push_firmware"));
}

TEST_CASE("On push failure with OpenThread restarted on the previous firmware: returns the push error in BR_RUNNING", "[mode_fsm]")
{
    setup(BR_MODE_BR_RUNNING);
    fake.push_restarted = true;
    fake.fail[OP_PUSH] = ESP_ERR_INVALID_SIZE;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, mode_fsm_push_firmware(&fsm, &push_source, push_detail, sizeof(push_detail)));
    TEST_ASSERT_EQUAL(BR_MODE_BR_RUNNING, mode_fsm_get_mode(&fsm));
}

TEST_CASE("On push when OpenThread does not come back: stays in RADIO_FAULT", "[mode_fsm]")
{
    setup(BR_MODE_BR_RUNNING);
    fake.push_restarted = false;
    fake.fail[OP_PUSH] = ESP_FAIL;
    TEST_ASSERT_EQUAL(ESP_FAIL, mode_fsm_push_firmware(&fsm, &push_source, push_detail, sizeof(push_detail)));
    TEST_ASSERT_EQUAL(BR_MODE_RADIO_FAULT, mode_fsm_get_mode(&fsm));
}

TEST_CASE("On push: the mode is STARTING while the job runs", "[mode_fsm]")
{
    setup(BR_MODE_BR_RUNNING);
    fake.push_restarted = true;
    TEST_ASSERT_EQUAL(ESP_OK, mode_fsm_push_firmware(&fsm, &push_source, push_detail, sizeof(push_detail)));
    TEST_ASSERT_EQUAL(BR_MODE_STARTING, fake.mode_in_job);
    TEST_ASSERT_EQUAL(BR_MODE_BR_RUNNING, mode_fsm_get_mode(&fsm));
}

TEST_CASE("On a begun radio job: a second begin is refused and the push runs the begun job", "[mode_fsm]")
{
    setup(BR_MODE_RADIO_FAULT);
    TEST_ASSERT_EQUAL(ESP_OK, mode_fsm_begin_radio_job(&fsm));
    TEST_ASSERT_EQUAL(BR_MODE_STARTING, mode_fsm_get_mode(&fsm));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, mode_fsm_begin_radio_job(&fsm));
    fake.push_restarted = true;
    TEST_ASSERT_EQUAL(ESP_OK, mode_fsm_push_firmware(&fsm, &push_source, push_detail, sizeof(push_detail)));
    TEST_ASSERT_EQUAL(1, count_of("push_firmware"));
    TEST_ASSERT_EQUAL(BR_MODE_BR_RUNNING, mode_fsm_get_mode(&fsm));
}

TEST_CASE("On the border router start inside a radio job: the mode stays STARTING until the job ends", "[mode_fsm]")
{
    setup(BR_MODE_BR_RUNNING);
    TEST_ASSERT_EQUAL(ESP_OK, mode_fsm_begin_radio_job(&fsm));
    TEST_ASSERT_FALSE(mode_fsm_set_started(&fsm));
    TEST_ASSERT_EQUAL(BR_MODE_STARTING, mode_fsm_get_mode(&fsm));
    mode_fsm_cancel_radio_job(&fsm);
}

TEST_CASE("On a cancelled radio job: the mode before it comes back, unless a fault came meanwhile", "[mode_fsm]")
{
    setup(BR_MODE_BR_RUNNING);
    TEST_ASSERT_EQUAL(ESP_OK, mode_fsm_begin_radio_job(&fsm));
    mode_fsm_cancel_radio_job(&fsm);
    TEST_ASSERT_EQUAL(BR_MODE_BR_RUNNING, mode_fsm_get_mode(&fsm));
    TEST_ASSERT_EQUAL(ESP_OK, mode_fsm_begin_radio_job(&fsm)); /* the job flag was cleared */
    atomic_store(&fsm.mode, BR_MODE_RADIO_FAULT);
    mode_fsm_cancel_radio_job(&fsm);
    TEST_ASSERT_EQUAL(BR_MODE_RADIO_FAULT, mode_fsm_get_mode(&fsm));
    TEST_ASSERT_EQUAL(0, fake.count);
}

TEST_CASE("On push without a source: returns ESP_ERR_INVALID_ARG and does not fall back to ROM recovery", "[mode_fsm]")
{
    setup(BR_MODE_BR_RUNNING);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, mode_fsm_push_firmware(&fsm, NULL, push_detail, sizeof(push_detail)));
    TEST_ASSERT_EQUAL(0, fake.count);
}

/* ---- ROM recovery ---- */

TEST_CASE("On ROM recovery in SNIFFER or STARTING: returns ESP_ERR_INVALID_STATE and starts nothing", "[mode_fsm]")
{
    setup(BR_MODE_SNIFFER);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, mode_fsm_rom_recover(&fsm, push_detail, sizeof(push_detail)));
    TEST_ASSERT_EQUAL(0, fake.count);
    TEST_ASSERT_EQUAL(BR_MODE_SNIFFER, mode_fsm_get_mode(&fsm));
    setup(BR_MODE_STARTING);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, mode_fsm_rom_recover(&fsm, push_detail, sizeof(push_detail)));
    TEST_ASSERT_EQUAL(0, fake.count);
}

TEST_CASE("On ROM recovery from BR_RUNNING or RADIO_FAULT with OpenThread running again: runs the recovery job once and enters BR_RUNNING", "[mode_fsm]")
{
    setup(BR_MODE_RADIO_FAULT);
    fake.push_restarted = true;
    TEST_ASSERT_EQUAL(ESP_OK, mode_fsm_rom_recover(&fsm, push_detail, sizeof(push_detail)));
    TEST_ASSERT_EQUAL(BR_MODE_BR_RUNNING, mode_fsm_get_mode(&fsm));
    TEST_ASSERT_EQUAL(1, count_of("rom_recover"));
    TEST_ASSERT_EQUAL(0, count_of("push_firmware"));
}

TEST_CASE("On ROM recovery failure with OpenThread restarted: returns the error in BR_RUNNING", "[mode_fsm]")
{
    setup(BR_MODE_BR_RUNNING);
    fake.push_restarted = true;
    fake.fail[OP_ROM_RECOVER] = ESP_FAIL;
    TEST_ASSERT_EQUAL(ESP_FAIL, mode_fsm_rom_recover(&fsm, push_detail, sizeof(push_detail)));
    TEST_ASSERT_EQUAL(BR_MODE_BR_RUNNING, mode_fsm_get_mode(&fsm));
}

TEST_CASE("On ROM recovery when OpenThread does not come back: stays in RADIO_FAULT", "[mode_fsm]")
{
    setup(BR_MODE_BR_RUNNING);
    fake.push_restarted = false;
    TEST_ASSERT_EQUAL(ESP_OK, mode_fsm_rom_recover(&fsm, push_detail, sizeof(push_detail)));
    TEST_ASSERT_EQUAL(BR_MODE_RADIO_FAULT, mode_fsm_get_mode(&fsm));
}

TEST_CASE("On push from BR_RUNNING or RADIO_FAULT on another task: push_allowed returns ESP_OK", "[mode_fsm]")
{
    TEST_ASSERT_EQUAL(ESP_OK, mode_fsm_push_allowed(BR_MODE_BR_RUNNING, false));
    TEST_ASSERT_EQUAL(ESP_OK, mode_fsm_push_allowed(BR_MODE_RADIO_FAULT, false));
}

TEST_CASE("On push in SNIFFER or STARTING: push_allowed returns ESP_ERR_INVALID_STATE", "[mode_fsm]")
{
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, mode_fsm_push_allowed(BR_MODE_SNIFFER, false));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, mode_fsm_push_allowed(BR_MODE_STARTING, false));
}

TEST_CASE("On push from the OpenThread task in any mode: push_allowed returns ESP_ERR_INVALID_STATE", "[mode_fsm]")
{
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, mode_fsm_push_allowed(BR_MODE_BR_RUNNING, true));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, mode_fsm_push_allowed(BR_MODE_RADIO_FAULT, true));
}

TEST_CASE("On the end of a push in BR_RUNNING with OpenThread up: the Thread REST flag is on", "[mode_fsm]")
{
    TEST_ASSERT_TRUE(mode_fsm_rest_ready_after_push(BR_MODE_BR_RUNNING, true));
}

TEST_CASE("On the end of a push in RADIO_FAULT or without OpenThread: the Thread REST flag is off", "[mode_fsm]")
{
    TEST_ASSERT_FALSE(mode_fsm_rest_ready_after_push(BR_MODE_RADIO_FAULT, true));
    TEST_ASSERT_FALSE(mode_fsm_rest_ready_after_push(BR_MODE_BR_RUNNING, false));
}

/* ---- storage ---- */

TEST_CASE("On a storage init that worked: the device is not degraded", "[mode_fsm]")
{
    TEST_ASSERT_FALSE(mode_fsm_storage_degraded(ESP_OK));
}

TEST_CASE("On NVS without free pages: the device is degraded", "[mode_fsm]")
{
    TEST_ASSERT_TRUE(mode_fsm_storage_degraded(0x1105 /* ESP_ERR_NVS_NO_FREE_PAGES */));
}

TEST_CASE("On any other storage init error: the device is degraded", "[mode_fsm]")
{
    TEST_ASSERT_TRUE(mode_fsm_storage_degraded(ESP_FAIL));
}

TEST_CASE("On a degraded storage: OpenThread may not start", "[mode_fsm]")
{
    TEST_ASSERT_FALSE(mode_fsm_radio_start_allowed(true));
}

TEST_CASE("On a healthy storage: OpenThread may start", "[mode_fsm]")
{
    TEST_ASSERT_TRUE(mode_fsm_radio_start_allowed(false));
}

/* ---- fault hook task ---- */

TEST_CASE("On an RCP failure hook running in the OpenThread task: the task is parked", "[mode_fsm]")
{
    TEST_ASSERT_EQUAL(MODE_FSM_FAULT_PARK_TASK, mode_fsm_fault_action(true));
}

TEST_CASE("On an RCP failure hook running in another task (httpd, thread_rest, pcap_stream, bring-up): the device reboots", "[mode_fsm]")
{
    TEST_ASSERT_EQUAL(MODE_FSM_FAULT_REBOOT, mode_fsm_fault_action(false));
}

/* ---- thread ops guard ---- */

TEST_CASE("On BR_RUNNING: thread_ops_allowed returns true", "[mode_fsm]")
{
    setup(BR_MODE_BR_RUNNING);
    TEST_ASSERT_TRUE(mode_fsm_thread_ops_allowed(&fsm));
}

TEST_CASE("On SNIFFER or RADIO_FAULT: thread_ops_allowed returns false", "[mode_fsm]")
{
    setup(BR_MODE_SNIFFER);
    TEST_ASSERT_FALSE(mode_fsm_thread_ops_allowed(&fsm));
    setup(BR_MODE_RADIO_FAULT);
    TEST_ASSERT_FALSE(mode_fsm_thread_ops_allowed(&fsm));
}

/* ---- sniffer end hook (R17) ---- */

TEST_CASE("On RCP failure while in SNIFFER: the sniffer end callback runs once, after the mode left SNIFFER", "[mode_fsm]")
{
    setup(BR_MODE_SNIFFER);
    mode_fsm_set_sniffer_end_cb(&fsm, end_cb);
    mode_fsm_on_rcp_failure(&fsm);
    TEST_ASSERT_EQUAL(1, end_cb_calls);
    TEST_ASSERT_EQUAL(BR_MODE_RADIO_FAULT, end_cb_mode);
}

TEST_CASE("On RCP failure in SNIFFER with no recovery allowed: the sniffer end callback still runs once", "[mode_fsm]")
{
    setup(BR_MODE_SNIFFER);
    mode_fsm_set_sniffer_end_cb(&fsm, end_cb);
    fsm.recovery_count = MODE_FSM_MAX_RECOVERIES;
    for (size_t i = 0; i < MODE_FSM_MAX_RECOVERIES; i++) {
        fsm.recovery_us[i] = 0;
    }
    mode_fsm_on_rcp_failure(&fsm);
    TEST_ASSERT_EQUAL(1, end_cb_calls);
}

TEST_CASE("On a radio fault while in SNIFFER: the sniffer end callback runs once", "[mode_fsm]")
{
    setup(BR_MODE_SNIFFER);
    mode_fsm_set_sniffer_end_cb(&fsm, end_cb);
    mode_fsm_set_fault(&fsm);
    TEST_ASSERT_EQUAL(1, end_cb_calls);
    TEST_ASSERT_EQUAL(BR_MODE_RADIO_FAULT, end_cb_mode);
    mode_fsm_set_fault(&fsm); /* already in RADIO_FAULT: nothing to end */
    TEST_ASSERT_EQUAL(1, end_cb_calls);
}

TEST_CASE("On RCP failure or radio fault outside SNIFFER: the sniffer end callback does not run", "[mode_fsm]")
{
    const br_core_mode_t modes[] = {BR_MODE_STARTING, BR_MODE_BR_RUNNING, BR_MODE_RADIO_FAULT};
    for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
        setup(modes[i]);
        mode_fsm_set_sniffer_end_cb(&fsm, end_cb);
        mode_fsm_on_rcp_failure(&fsm);
        mode_fsm_set_fault(&fsm);
        TEST_ASSERT_EQUAL(0, end_cb_calls);
    }
}

TEST_CASE("On leave_sniffer: the sniffer end callback does not run", "[mode_fsm]")
{
    setup(BR_MODE_SNIFFER);
    mode_fsm_set_sniffer_end_cb(&fsm, end_cb);
    TEST_ASSERT_EQUAL(ESP_OK, mode_fsm_leave_sniffer(&fsm));
    TEST_ASSERT_EQUAL(0, end_cb_calls);
}

TEST_CASE("On a second capture after an ended one: the sniffer end callback runs again", "[mode_fsm]")
{
    setup(BR_MODE_BR_RUNNING);
    mode_fsm_set_sniffer_end_cb(&fsm, end_cb);
    for (int round = 1; round <= 2; round++) {
        TEST_ASSERT_EQUAL(ESP_OK, mode_fsm_enter_sniffer(&fsm, 15, rx_done));
        mode_fsm_on_rcp_failure(&fsm);
        run_deferred();
        TEST_ASSERT_EQUAL(round, end_cb_calls);
        TEST_ASSERT_EQUAL(BR_MODE_BR_RUNNING, mode_fsm_get_mode(&fsm));
    }
}

TEST_CASE("On RCP failure in SNIFFER without a registered callback: ends the capture without crashing", "[mode_fsm]")
{
    setup(BR_MODE_SNIFFER);
    mode_fsm_on_rcp_failure(&fsm);
    run_deferred();
    TEST_ASSERT_EQUAL(BR_MODE_BR_RUNNING, mode_fsm_get_mode(&fsm));
}

TEST_CASE("On a push whose version was read back: the new version is known", "[br_core]")
{
    bool known = false;
    char installed[32] = "1.0.0";

    mode_fsm_installed_after_push(true, "2.0.0", &known, installed, sizeof(installed));
    TEST_ASSERT_TRUE(known);
    TEST_ASSERT_EQUAL_STRING("2.0.0", installed);
}

TEST_CASE("On a push whose version could not be read back: unknown and the old version is gone", "[br_core]")
{
    bool known = true;
    char installed[32] = "1.0.0";

    mode_fsm_installed_after_push(false, "", &known, installed, sizeof(installed));
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

TEST_CASE("On a failed ROM recovery after a silent updater: the boot enters RADIO_FAULT", "[mode_fsm]")
{
    rcp_sync_probe_result_t probe = {.rom_recovery_failed = true, .updater_reachable = false};

    TEST_ASSERT_TRUE(mode_fsm_rom_failure_is_fault(&probe));
}

TEST_CASE("On a failed ROM recovery after a version mismatch (updater reachable): the boot goes on with the installed RCP", "[mode_fsm]")
{
    rcp_sync_probe_result_t probe = {.rom_recovery_failed = true, .updater_reachable = true, .installed_valid = true};

    TEST_ASSERT_FALSE(mode_fsm_rom_failure_is_fault(&probe));
}

TEST_CASE("On a probe without a failed recovery: no fault from the recovery", "[mode_fsm]")
{
    rcp_sync_probe_result_t probe = {.updater_reachable = false};

    TEST_ASSERT_FALSE(mode_fsm_rom_failure_is_fault(&probe));
}

TEST_CASE("On an automatic recovery failing and then a manual one succeeding: the failed flag is cleared", "[mode_fsm]")
{
    bool flag = mode_fsm_next_recovery_failed(false, true, false);

    TEST_ASSERT_TRUE(flag);
    flag = mode_fsm_next_recovery_failed(flag, true, true);
    TEST_ASSERT_FALSE(flag);
}

TEST_CASE("On a manual recovery failing: the failed flag is set", "[mode_fsm]")
{
    TEST_ASSERT_TRUE(mode_fsm_next_recovery_failed(false, true, false));
}

TEST_CASE("On a probe-time recovery succeeding after an earlier failure: the failed flag is cleared", "[mode_fsm]")
{
    TEST_ASSERT_FALSE(mode_fsm_next_recovery_failed(true, true, true));
}

TEST_CASE("On no recovery attempt: the failed flag is unchanged", "[mode_fsm]")
{
    TEST_ASSERT_TRUE(mode_fsm_next_recovery_failed(true, false, false));
    TEST_ASSERT_FALSE(mode_fsm_next_recovery_failed(false, false, false));
}

/* ---- result of a C6 job ---- */

TEST_CASE("On a ROM recovery that worked and a failed restart: the job result is ESP_OK", "[mode_fsm]")
{
    TEST_ASSERT_EQUAL(ESP_OK, mode_fsm_radio_job_result(true, ESP_OK, ESP_FAIL));
}

TEST_CASE("On a c6_installer push that worked (base only) and a failed restart: the job result is ESP_OK", "[mode_fsm]")
{
    /* br_core passes base_only for an installer push as for a recovery: both leave the C6 without RCP firmware. */
    TEST_ASSERT_EQUAL(ESP_OK, mode_fsm_radio_job_result(true, ESP_OK, ESP_ERR_TIMEOUT));
}

TEST_CASE("On a ROM recovery that failed: the job result is its error, whatever the restart did", "[mode_fsm]")
{
    TEST_ASSERT_EQUAL(ESP_FAIL, mode_fsm_radio_job_result(true, ESP_FAIL, ESP_OK));
    TEST_ASSERT_EQUAL(ESP_FAIL, mode_fsm_radio_job_result(true, ESP_FAIL, ESP_ERR_TIMEOUT));
}

TEST_CASE("On a firmware push that worked and a failed restart: the job result is the restart error", "[mode_fsm]")
{
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, mode_fsm_radio_job_result(false, ESP_OK, ESP_ERR_TIMEOUT));
}

TEST_CASE("On a firmware push that failed: the job result is the push error", "[mode_fsm]")
{
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_CRC, mode_fsm_radio_job_result(false, ESP_ERR_INVALID_CRC, ESP_FAIL));
}
