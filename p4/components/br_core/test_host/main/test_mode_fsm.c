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
    esp_err_t fail[24]; /* per op name index, see OP_* */
    int64_t now;
    uint8_t raw_channel;
    otLinkRawReceiveDone raw_cb;
    void (*deferred_fn)(void *ctx); /* what the defer op was given, run by run_deferred() */
    void *deferred_ctx;
    const char *start_fail_on[2]; /* channel ids whose ot_start fails */
    esp_err_t start_fail_code;    /* what it returns then (ESP_FAIL when 0) */
    const char *abandon_on;       /* channel id whose ot_start abandons the init (MODE_FSM_ERR_START_ABANDONED) */
    bool start_reaches_running;   /* ot_start ends like the border router start: STARTING to BR_RUNNING */
    char started_on[48];          /* the id of the last ot_start that worked */
    char stored_from[48], stored_to[48];
    br_core_mode_t mode_in_stop; /* the mode the ot_stop op saw */
    bool fault_in_stop;          /* ot_stop enters RADIO_FAULT while it runs (a park during the lost drain) */
    char probed[48];             /* the id of the last probe_channel ("" for a stop's acceptance) */
    esp_err_t reset_first;       /* what the first reset_rcp returns (fail[OP_RESET_RCP] for the later ones) */
    int resets;                  /* reset_rcp calls so far */
    uint32_t slept_ms;           /* what sleep_ms was given, summed */
    br_core_mode_t heard[LOG_CAP]; /* the modes the mode_changed op heard, in order */
    int heard_count;
} fake_t;

enum { OP_THREAD_STOP, OP_NETIF_DOWN, OP_RAW_ENABLE, OP_RAW_DISABLE, OP_RESTORE_CHANNEL, OP_NETIF_UP, OP_THREAD_START,
       OP_RESET_RCP, OP_RAW_ABORT, OP_QUIET, OP_DEFER, OP_PROBE, OP_OT_STOP, OP_OT_START,
       OP_STORE, OP_COUNT
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
    const esp_err_t ret = record(OP_RESET_RCP, "reset_rcp");

    return fake.resets++ == 0 && fake.reset_first != ESP_OK ? fake.reset_first : ret;
}
static void f_sleep_ms(uint32_t ms)
{
    fake.slept_ms += ms;
    record(OP_COUNT, "sleep_ms");
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
static esp_err_t f_probe_channel(const char *id, char *err, size_t cap)
{
    snprintf(fake.probed, sizeof(fake.probed), "%s", id);
    const esp_err_t ret = record(OP_PROBE, "probe_channel");

    if (ret != ESP_OK) {
        snprintf(err, cap, "no spinel answer");
    }
    return ret;
}
static esp_err_t f_ot_stop(void)
{
    fake.mode_in_stop = mode_fsm_get_mode(&fsm);
    if (fake.fault_in_stop) {
        mode_fsm_set_fault(&fsm); /* an OpenThread task parked by its failure hook during the drain */
    }
    return record(OP_OT_STOP, "ot_stop");
}
static esp_err_t f_ot_start(const char *id)
{
    esp_err_t ret = record(OP_OT_START, "ot_start");

    for (int i = 0; i < 2; i++) {
        if (fake.start_fail_on[i] && strcmp(fake.start_fail_on[i], id) == 0) {
            ret = fake.start_fail_code ? fake.start_fail_code : ESP_FAIL;
        }
    }
    if (fake.abandon_on && strcmp(fake.abandon_on, id) == 0) {
        ret = MODE_FSM_ERR_START_ABANDONED;
    }
    if (ret == ESP_OK) {
        snprintf(fake.started_on, sizeof(fake.started_on), "%s", id);
        if (fake.start_reaches_running) {
            mode_fsm_set_started(&fsm); /* the border router start that follows a working OpenThread start */
        }
    }
    return ret;
}
static esp_err_t f_store_otbr(const char *from, const char *to)
{
    snprintf(fake.stored_from, sizeof(fake.stored_from), "%s", from);
    snprintf(fake.stored_to, sizeof(fake.stored_to), "%s", to);
    return record(OP_STORE, "store_otbr");
}

static void f_mode_changed(br_core_mode_t mode)
{
    if (fake.heard_count < LOG_CAP) {
        fake.heard[fake.heard_count++] = mode;
    }
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
    .reboot = f_reboot,
    .now_us = f_now,
    .sleep_ms = f_sleep_ms,
    .probe_channel = f_probe_channel,
    .ot_start = f_ot_start,
    .ot_stop = f_ot_stop,
    .store_otbr = f_store_otbr,
    .mode_changed = f_mode_changed,
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
    fake.start_reaches_running = true;
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

TEST_CASE("On RCP failure when the reset fails: stays in RADIO_FAULT", "[mode_fsm]")
{
    setup(BR_MODE_BR_RUNNING);
    fake.fail[OP_RESET_RCP] = ESP_FAIL;
    mode_fsm_on_rcp_failure(&fsm);
    TEST_ASSERT_EQUAL(BR_MODE_RADIO_FAULT, mode_fsm_get_mode(&fsm));
    TEST_ASSERT_EQUAL(1, count_of("reset_rcp"));
}

TEST_CASE("On RCP failure when the mux could not take the reset (timeout): tries once more after a short wait",
          "[mode_fsm]")
{
    setup(BR_MODE_BR_RUNNING);
    fake.reset_first = ESP_ERR_TIMEOUT;
    mode_fsm_on_rcp_failure(&fsm);
    TEST_ASSERT_EQUAL(2, count_of("reset_rcp"));
    TEST_ASSERT_EQUAL_UINT32(MODE_FSM_RESET_RETRY_MS, fake.slept_ms);
    const int first = index_of("reset_rcp"), sleep = index_of("sleep_ms");

    TEST_ASSERT_TRUE(first < sleep);
    TEST_ASSERT_EQUAL_STRING("reset_rcp", fake.calls[sleep + 1]); // the retry comes after the wait
}

TEST_CASE("On RCP failure when the mux could not take the reset and the retry works: enters BR_RUNNING",
          "[mode_fsm]")
{
    setup(BR_MODE_BR_RUNNING);
    fake.reset_first = ESP_ERR_NO_MEM;
    mode_fsm_on_rcp_failure(&fsm);
    TEST_ASSERT_EQUAL(BR_MODE_BR_RUNNING, mode_fsm_get_mode(&fsm));
}

TEST_CASE("On RCP failure when the reset's retry is not taken either: stays in RADIO_FAULT", "[mode_fsm]")
{
    setup(BR_MODE_BR_RUNNING);
    fake.reset_first = ESP_ERR_TIMEOUT;
    fake.fail[OP_RESET_RCP] = ESP_ERR_TIMEOUT;
    mode_fsm_on_rcp_failure(&fsm);
    TEST_ASSERT_EQUAL(BR_MODE_RADIO_FAULT, mode_fsm_get_mode(&fsm));
    TEST_ASSERT_EQUAL(2, count_of("reset_rcp"));
}

TEST_CASE("On RCP failure when the reset fails otherwise: no retry, no wait", "[mode_fsm]")
{
    setup(BR_MODE_BR_RUNNING);
    fake.fail[OP_RESET_RCP] = ESP_ERR_INVALID_STATE;
    mode_fsm_on_rcp_failure(&fsm);
    TEST_ASSERT_EQUAL(1, count_of("reset_rcp"));
    TEST_ASSERT_EQUAL(0, count_of("sleep_ms"));
}

TEST_CASE("On RCP failure while STARTING when the mux could not take the reset: tries once more", "[mode_fsm]")
{
    setup(BR_MODE_STARTING);
    fake.reset_first = ESP_ERR_NO_MEM;
    mode_fsm_on_rcp_failure(&fsm);
    TEST_ASSERT_EQUAL(2, count_of("reset_rcp"));
    TEST_ASSERT_EQUAL(BR_MODE_STARTING, mode_fsm_get_mode(&fsm));
}

TEST_CASE("On mode_fsm_reset_rcp with a reset taken at once: one reset, no wait", "[mode_fsm]")
{
    setup(BR_MODE_BR_RUNNING);
    TEST_ASSERT_EQUAL(ESP_OK, mode_fsm_reset_rcp(&fsm));
    TEST_ASSERT_EQUAL(1, count_of("reset_rcp"));
    TEST_ASSERT_EQUAL_UINT32(0, fake.slept_ms);
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

void app_main(void)
{
    UNITY_BEGIN();
    unity_run_all_tests();
    int failures = (int)UNITY_END();
    exit(failures == 0 ? 0 : 1);
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

TEST_CASE("On a firmware push that worked and a failed restart: the job result is RCP_SYNC_ERR_RESTART", "[mode_fsm]")
{
    TEST_ASSERT_EQUAL(RCP_SYNC_ERR_RESTART, mode_fsm_radio_job_result(false, ESP_OK, ESP_ERR_TIMEOUT));
    TEST_ASSERT_EQUAL(RCP_SYNC_ERR_RESTART, mode_fsm_radio_job_result(false, ESP_OK, ESP_FAIL));
}

TEST_CASE("On a firmware push that worked and a restart that worked: the job result is ESP_OK", "[mode_fsm]")
{
    TEST_ASSERT_EQUAL(ESP_OK, mode_fsm_radio_job_result(false, ESP_OK, ESP_OK));
}

TEST_CASE("On a firmware push that failed: the job result is the push error", "[mode_fsm]")
{
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_CRC, mode_fsm_radio_job_result(false, ESP_ERR_INVALID_CRC, ESP_FAIL));
}

/* ---- moving OpenThread to another channel ---- */

#define SRC "builtin-c6"
#define DST "usb-1a86-55d4"

TEST_CASE("On a probe failure of the target: refuses with probe_failed and the probe's reason", "[mode_fsm]")
{
    char err[64] = "";

    setup(BR_MODE_BR_RUNNING);
    fake.fail[OP_PROBE] = ESP_FAIL;
    TEST_ASSERT_EQUAL(MODE_FSM_ERR_PROBE_FAILED, mode_fsm_move_otbr(&fsm, SRC, DST, err, sizeof(err)));
    TEST_ASSERT_EQUAL_STRING("no spinel answer", err);
}

TEST_CASE("On a probe failure of the target: no stop, no start, nothing stored, the mode stays", "[mode_fsm]")
{
    char err[64];

    setup(BR_MODE_BR_RUNNING);
    fake.fail[OP_PROBE] = ESP_FAIL;
    mode_fsm_move_otbr(&fsm, SRC, DST, err, sizeof(err));
    TEST_ASSERT_EQUAL(0, count_of("ot_stop"));
    TEST_ASSERT_EQUAL(0, count_of("ot_start"));
    TEST_ASSERT_EQUAL(0, count_of("store_otbr"));
    TEST_ASSERT_EQUAL(BR_MODE_BR_RUNNING, mode_fsm_get_mode(&fsm));
}

TEST_CASE("On a successful move: probes the target, stops, stores both modes (the commit), then starts on the target",
          "[mode_fsm]")
{
    char err[64];

    setup(BR_MODE_BR_RUNNING);
    TEST_ASSERT_EQUAL(ESP_OK, mode_fsm_move_otbr(&fsm, SRC, DST, err, sizeof(err)));
    assert_order("probe_channel", "ot_stop");
    assert_order("ot_stop", "store_otbr");
    assert_order("store_otbr", "ot_start");
    TEST_ASSERT_EQUAL_STRING(DST, fake.started_on);
    TEST_ASSERT_EQUAL_STRING(SRC, fake.stored_from);
    TEST_ASSERT_EQUAL_STRING(DST, fake.stored_to);
}

TEST_CASE("On a successful move: STARTING while OpenThread is stopped, ends in BR_RUNNING", "[mode_fsm]")
{
    char err[64];

    setup(BR_MODE_BR_RUNNING);
    mode_fsm_move_otbr(&fsm, SRC, DST, err, sizeof(err));
    TEST_ASSERT_EQUAL(BR_MODE_STARTING, fake.mode_in_stop);
    TEST_ASSERT_EQUAL(BR_MODE_BR_RUNNING, mode_fsm_get_mode(&fsm));
}

TEST_CASE("On a move from an absent source (NO_RADIO): starts on the target, stores, ends in BR_RUNNING",
          "[mode_fsm]")
{
    char err[64];

    setup(BR_MODE_NO_RADIO);
    TEST_ASSERT_EQUAL(ESP_OK, mode_fsm_move_otbr(&fsm, SRC, DST, err, sizeof(err)));
    TEST_ASSERT_EQUAL_STRING(DST, fake.started_on);
    TEST_ASSERT_EQUAL(1, count_of("store_otbr"));
    TEST_ASSERT_EQUAL(BR_MODE_BR_RUNNING, mode_fsm_get_mode(&fsm));
}

TEST_CASE("On a start failure on the target: RADIO_FAULT on the target, the binding stays there, no start back on the "
          "source", "[mode_fsm]")
{
    char err[64] = "";

    setup(BR_MODE_BR_RUNNING);
    fake.start_fail_on[0] = DST;
    TEST_ASSERT_EQUAL(MODE_FSM_ERR_START_FAILED, mode_fsm_move_otbr(&fsm, SRC, DST, err, sizeof(err)));
    TEST_ASSERT_EQUAL(1, count_of("ot_start")); /* the target's only: no rollback */
    TEST_ASSERT_EQUAL_STRING("", fake.started_on);
    TEST_ASSERT_EQUAL_STRING(DST, fake.stored_to);
    TEST_ASSERT_EQUAL_STRING(SRC, fake.stored_from);
    TEST_ASSERT_EQUAL(BR_MODE_RADIO_FAULT, mode_fsm_get_mode(&fsm));
    TEST_ASSERT_EQUAL(0, count_of("reboot"));
    TEST_ASSERT_NOT_EQUAL(0, strlen(err));
}

TEST_CASE("On a start failure on the target from an absent source: RADIO_FAULT on the target, the binding there",
          "[mode_fsm]")
{
    char err[64];

    setup(BR_MODE_NO_RADIO);
    fake.start_fail_on[0] = DST;
    TEST_ASSERT_EQUAL(MODE_FSM_ERR_START_FAILED, mode_fsm_move_otbr(&fsm, SRC, DST, err, sizeof(err)));
    TEST_ASSERT_EQUAL(1, count_of("ot_start"));
    TEST_ASSERT_EQUAL_STRING(DST, fake.stored_to);
    TEST_ASSERT_EQUAL(BR_MODE_RADIO_FAULT, mode_fsm_get_mode(&fsm));
}

TEST_CASE("On a target start that abandoned OpenThread's init: RADIO_FAULT with its text, the binding on the target, no "
          "start on the source, no reboot", "[mode_fsm]")
{
    char err[96] = "";

    setup(BR_MODE_BR_RUNNING);
    fake.start_fail_on[0] = DST;
    fake.start_fail_code = MODE_FSM_ERR_START_ABANDONED;
    TEST_ASSERT_EQUAL(MODE_FSM_ERR_START_ABANDONED, mode_fsm_move_otbr(&fsm, SRC, DST, err, sizeof(err)));
    TEST_ASSERT_EQUAL(1, count_of("ot_start"));
    TEST_ASSERT_EQUAL_STRING(DST, fake.stored_to);
    TEST_ASSERT_EQUAL(BR_MODE_RADIO_FAULT, mode_fsm_get_mode(&fsm));
    TEST_ASSERT_EQUAL(0, count_of("reboot"));
    TEST_ASSERT_EQUAL_STRING(MODE_FSM_FAULT_ABANDONED, err);
    TEST_ASSERT_EQUAL_STRING("an earlier OpenThread start was abandoned: reboot", MODE_FSM_FAULT_ABANDONED);
}

TEST_CASE("On a store failure (the commit): nothing changed, the target never started, OpenThread starts again on the "
          "source", "[mode_fsm]")
{
    char err[96] = "";

    setup(BR_MODE_BR_RUNNING);
    fake.fail[OP_STORE] = ESP_ERR_NO_MEM;
    TEST_ASSERT_EQUAL(MODE_FSM_ERR_START_FAILED, mode_fsm_move_otbr(&fsm, SRC, DST, err, sizeof(err)));
    TEST_ASSERT_EQUAL(1, count_of("ot_stop"));
    TEST_ASSERT_EQUAL(1, count_of("ot_start"));
    TEST_ASSERT_EQUAL_STRING(SRC, fake.started_on);
    TEST_ASSERT_EQUAL(BR_MODE_BR_RUNNING, mode_fsm_get_mode(&fsm));
    TEST_ASSERT_NOT_EQUAL(0, strlen(err));
}

TEST_CASE("On a drain timeout: refuses with drain_timeout", "[mode_fsm]")
{
    char err[64] = "";

    setup(BR_MODE_BR_RUNNING);
    fake.fail[OP_OT_STOP] = ESP_ERR_TIMEOUT;
    TEST_ASSERT_EQUAL(MODE_FSM_ERR_DRAIN_TIMEOUT, mode_fsm_move_otbr(&fsm, SRC, DST, err, sizeof(err)));
    TEST_ASSERT_EQUAL_STRING(MODE_FSM_DRAIN_TIMEOUT, err);
}

TEST_CASE("On a drain timeout: nothing changed (no start, nothing stored, the mode before the move)", "[mode_fsm]")
{
    char err[64];

    setup(BR_MODE_BR_RUNNING);
    fake.fail[OP_OT_STOP] = ESP_ERR_TIMEOUT;
    mode_fsm_move_otbr(&fsm, SRC, DST, err, sizeof(err));
    TEST_ASSERT_EQUAL(0, count_of("ot_start"));
    TEST_ASSERT_EQUAL(0, count_of("store_otbr"));
    TEST_ASSERT_EQUAL(BR_MODE_BR_RUNNING, mode_fsm_get_mode(&fsm));
}

TEST_CASE("On a move in STARTING or SNIFFER: refuses as busy, nothing probed", "[mode_fsm]")
{
    const br_core_mode_t modes[] = {BR_MODE_STARTING, BR_MODE_SNIFFER};
    char err[64];

    for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
        setup(modes[i]);
        TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, mode_fsm_move_otbr(&fsm, SRC, DST, err, sizeof(err)));
        TEST_ASSERT_EQUAL(0, fake.count);
        TEST_ASSERT_EQUAL(modes[i], mode_fsm_get_mode(&fsm));
    }
}

TEST_CASE("On a move from RADIO_FAULT: stops, starts on the target, stores, ends in BR_RUNNING", "[mode_fsm]")
{
    char err[64];

    setup(BR_MODE_RADIO_FAULT);
    TEST_ASSERT_EQUAL(ESP_OK, mode_fsm_move_otbr(&fsm, SRC, DST, err, sizeof(err)));
    TEST_ASSERT_EQUAL(1, count_of("store_otbr"));
    TEST_ASSERT_EQUAL(BR_MODE_BR_RUNNING, mode_fsm_get_mode(&fsm));
}

/* ---- a plain stop (the OTBR channel leaves otbr) and a plain start (no channel has otbr) ---- */

TEST_CASE("On a stop: accepted without probing a radio, OpenThread stopped, the source stored, ends in NO_RADIO",
          "[mode_fsm]")
{
    char err[64];

    setup(BR_MODE_BR_RUNNING);
    TEST_ASSERT_EQUAL(ESP_OK, mode_fsm_move_otbr(&fsm, SRC, "", err, sizeof(err)));
    TEST_ASSERT_EQUAL_STRING("", fake.probed); /* the acceptance only */
    assert_order("probe_channel", "ot_stop");
    assert_order("ot_stop", "store_otbr");
    TEST_ASSERT_EQUAL(0, count_of("ot_start"));
    TEST_ASSERT_EQUAL_STRING(SRC, fake.stored_from);
    TEST_ASSERT_EQUAL_STRING("", fake.stored_to);
    TEST_ASSERT_EQUAL(BR_MODE_STARTING, fake.mode_in_stop);
    TEST_ASSERT_EQUAL(BR_MODE_NO_RADIO, mode_fsm_get_mode(&fsm));
}

TEST_CASE("On a stop whose sessions do not end: refused with drain_timeout, nothing stored, the mode back",
          "[mode_fsm]")
{
    char err[64];

    setup(BR_MODE_BR_RUNNING);
    fake.fail[OP_OT_STOP] = ESP_ERR_TIMEOUT;
    TEST_ASSERT_EQUAL(MODE_FSM_ERR_DRAIN_TIMEOUT, mode_fsm_move_otbr(&fsm, SRC, "", err, sizeof(err)));
    TEST_ASSERT_EQUAL_STRING(MODE_FSM_DRAIN_TIMEOUT, err);
    TEST_ASSERT_EQUAL(0, count_of("store_otbr"));
    TEST_ASSERT_EQUAL(0, count_of("ot_start"));
    TEST_ASSERT_EQUAL(BR_MODE_BR_RUNNING, mode_fsm_get_mode(&fsm)); /* never RADIO_FAULT */
}

TEST_CASE("On a stop whose store fails: OpenThread starts again on the source, nothing stored", "[mode_fsm]")
{
    char err[64];

    setup(BR_MODE_BR_RUNNING);
    fake.fail[OP_STORE] = ESP_FAIL;
    TEST_ASSERT_EQUAL(MODE_FSM_ERR_START_FAILED, mode_fsm_move_otbr(&fsm, SRC, "", err, sizeof(err)));
    assert_order("store_otbr", "ot_start");
    TEST_ASSERT_EQUAL_STRING(SRC, fake.started_on);
    TEST_ASSERT_EQUAL(BR_MODE_BR_RUNNING, mode_fsm_get_mode(&fsm));
}

TEST_CASE("On a stop during STARTING or SNIFFER: busy, nothing touched", "[mode_fsm]")
{
    char err[64];

    setup(BR_MODE_STARTING);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, mode_fsm_move_otbr(&fsm, SRC, "", err, sizeof(err)));
    setup(BR_MODE_SNIFFER);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, mode_fsm_move_otbr(&fsm, SRC, "", err, sizeof(err)));
    TEST_ASSERT_EQUAL(0, fake.count);
}

TEST_CASE("On a start (no source): probes the target, starts on it, stores it, ends in BR_RUNNING", "[mode_fsm]")
{
    char err[64];

    setup(BR_MODE_NO_RADIO);
    TEST_ASSERT_EQUAL(ESP_OK, mode_fsm_move_otbr(&fsm, "", DST, err, sizeof(err)));
    TEST_ASSERT_EQUAL_STRING(DST, fake.probed);
    TEST_ASSERT_EQUAL_STRING(DST, fake.started_on);
    TEST_ASSERT_EQUAL_STRING("", fake.stored_from);
    TEST_ASSERT_EQUAL_STRING(DST, fake.stored_to);
    TEST_ASSERT_EQUAL(BR_MODE_BR_RUNNING, mode_fsm_get_mode(&fsm));
}

TEST_CASE("On a start (no source) that fails: the binding on the target, RADIO_FAULT, also after a fault", "[mode_fsm]")
{
    char err[64];

    setup(BR_MODE_NO_RADIO);
    fake.start_fail_on[0] = DST;
    TEST_ASSERT_EQUAL(MODE_FSM_ERR_START_FAILED, mode_fsm_move_otbr(&fsm, "", DST, err, sizeof(err)));
    TEST_ASSERT_EQUAL(1, count_of("store_otbr"));
    TEST_ASSERT_EQUAL_STRING(DST, fake.stored_to);
    TEST_ASSERT_EQUAL(1, count_of("ot_start"));
    TEST_ASSERT_EQUAL(BR_MODE_RADIO_FAULT, mode_fsm_get_mode(&fsm));
    setup(BR_MODE_RADIO_FAULT); /* a failed start earlier */
    fake.start_fail_on[0] = DST;
    TEST_ASSERT_EQUAL(MODE_FSM_ERR_START_FAILED, mode_fsm_move_otbr(&fsm, "", DST, err, sizeof(err)));
    TEST_ASSERT_EQUAL(1, count_of("ot_start"));
    TEST_ASSERT_EQUAL(BR_MODE_RADIO_FAULT, mode_fsm_get_mode(&fsm));
}

TEST_CASE("On a start (no source) whose store fails: nothing to go back to, NO_RADIO", "[mode_fsm]")
{
    char err[64];

    setup(BR_MODE_NO_RADIO);
    fake.fail[OP_STORE] = ESP_ERR_NO_MEM;
    TEST_ASSERT_EQUAL(MODE_FSM_ERR_START_FAILED, mode_fsm_move_otbr(&fsm, "", DST, err, sizeof(err)));
    TEST_ASSERT_EQUAL(0, count_of("ot_start"));
    TEST_ASSERT_EQUAL(BR_MODE_NO_RADIO, mode_fsm_get_mode(&fsm));
}

/* ---- the OTBR channel goes and comes back ---- */

TEST_CASE("On the OTBR channel gone: tears OpenThread down, ends in NO_RADIO", "[mode_fsm]")
{
    setup(BR_MODE_BR_RUNNING);
    mode_fsm_on_channel_gone(&fsm);
    TEST_ASSERT_EQUAL(1, count_of("ot_stop"));
    TEST_ASSERT_EQUAL(BR_MODE_NO_RADIO, mode_fsm_get_mode(&fsm));
}

TEST_CASE("On the OTBR channel gone: NO_RADIO already while OpenThread is torn down (new sessions refused)",
          "[mode_fsm]")
{
    setup(BR_MODE_BR_RUNNING);
    mode_fsm_on_channel_gone(&fsm);
    TEST_ASSERT_EQUAL(BR_MODE_NO_RADIO, fake.mode_in_stop);
    TEST_ASSERT_FALSE(mode_fsm_thread_ops_allowed(&fsm));
}

TEST_CASE("On the OTBR channel gone while sniffing: the capture owner is told, ends in NO_RADIO", "[mode_fsm]")
{
    setup(BR_MODE_SNIFFER);
    mode_fsm_set_sniffer_end_cb(&fsm, end_cb);
    mode_fsm_on_channel_gone(&fsm);
    TEST_ASSERT_EQUAL(1, end_cb_calls);
    TEST_ASSERT_EQUAL(1, count_of("ot_stop"));
    TEST_ASSERT_EQUAL(BR_MODE_NO_RADIO, mode_fsm_get_mode(&fsm));
}

TEST_CASE("On the OTBR channel gone and a fault entered during the teardown: ends in NO_RADIO", "[mode_fsm]")
{
    setup(BR_MODE_BR_RUNNING);
    fake.fault_in_stop = true;
    mode_fsm_on_channel_gone(&fsm);
    TEST_ASSERT_EQUAL(1, count_of("ot_stop"));
    TEST_ASSERT_EQUAL(BR_MODE_NO_RADIO, mode_fsm_get_mode(&fsm));
}

TEST_CASE("On the OTBR channel gone and a teardown that did not finish: ends in RADIO_FAULT", "[mode_fsm]")
{
    setup(BR_MODE_BR_RUNNING);
    fake.fail[OP_OT_STOP] = ESP_ERR_TIMEOUT;
    mode_fsm_on_channel_gone(&fsm);
    TEST_ASSERT_EQUAL(BR_MODE_RADIO_FAULT, mode_fsm_get_mode(&fsm));
}

TEST_CASE("On the OTBR channel present again: probes and starts OpenThread on it, ends in BR_RUNNING", "[mode_fsm]")
{
    setup(BR_MODE_NO_RADIO);
    mode_fsm_on_channel_back(&fsm, DST);
    assert_order("probe_channel", "ot_start");
    TEST_ASSERT_EQUAL_STRING(DST, fake.started_on);
    TEST_ASSERT_EQUAL(0, count_of("store_otbr"));
    TEST_ASSERT_EQUAL(BR_MODE_BR_RUNNING, mode_fsm_get_mode(&fsm));
}

TEST_CASE("On the OTBR channel present again and a failed probe: no start, ends in RADIO_FAULT", "[mode_fsm]")
{
    setup(BR_MODE_NO_RADIO);
    fake.fail[OP_PROBE] = ESP_FAIL;
    mode_fsm_on_channel_back(&fsm, DST);
    TEST_ASSERT_EQUAL(0, count_of("ot_start"));
    TEST_ASSERT_EQUAL(BR_MODE_RADIO_FAULT, mode_fsm_get_mode(&fsm));
}

TEST_CASE("On the OTBR channel present again and a failed start: ends in RADIO_FAULT", "[mode_fsm]")
{
    setup(BR_MODE_NO_RADIO);
    fake.start_fail_on[0] = DST;
    mode_fsm_on_channel_back(&fsm, DST);
    TEST_ASSERT_EQUAL(BR_MODE_RADIO_FAULT, mode_fsm_get_mode(&fsm));
}

/* ---- boot ---- */

TEST_CASE("On boot with no channel in otbr: ends in NO_RADIO, no fault, nothing started", "[mode_fsm]")
{
    setup(BR_MODE_STARTING);
    TEST_ASSERT_FALSE(mode_fsm_boot(&fsm, NULL));
    TEST_ASSERT_EQUAL(BR_MODE_NO_RADIO, mode_fsm_get_mode(&fsm));
    TEST_ASSERT_EQUAL(0, fake.count);
}

TEST_CASE("On boot with a dongle in otbr: NO_RADIO until it attaches, nothing started", "[mode_fsm]")
{
    setup(BR_MODE_STARTING);
    TEST_ASSERT_FALSE(mode_fsm_boot(&fsm, DST));
    TEST_ASSERT_EQUAL(BR_MODE_NO_RADIO, mode_fsm_get_mode(&fsm));
    TEST_ASSERT_EQUAL(0, fake.count);
}

TEST_CASE("On the built-in channel's id: builtin-c6 (the stored c6 is mapped by the radio mux)", "[mode_fsm]")
{
    TEST_ASSERT_EQUAL_STRING("builtin-c6", MODE_FSM_BUILTIN_ID);
    TEST_ASSERT_FALSE(mode_fsm_probe_fault_incompatible("c6", ESP_FAIL)); /* no channel has that id any more */
}

TEST_CASE("On boot with the C6 in otbr (default): the C6 boot goes on as today, mode STARTING", "[mode_fsm]")
{
    setup(BR_MODE_STARTING);
    TEST_ASSERT_TRUE(mode_fsm_boot(&fsm, MODE_FSM_BUILTIN_ID));
    TEST_ASSERT_EQUAL(BR_MODE_STARTING, mode_fsm_get_mode(&fsm));
    TEST_ASSERT_EQUAL(0, fake.count);
}

TEST_CASE("On NO_RADIO: Thread operations are refused", "[mode_fsm]")
{
    setup(BR_MODE_NO_RADIO);
    TEST_ASSERT_FALSE(mode_fsm_thread_ops_allowed(&fsm));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, mode_fsm_enter_sniffer(&fsm, 15, rx_done));
}

/* ---- the C6's RCP probe before a start (V9, the empty-probe regression) ---- */

TEST_CASE("On the C6's RCP probe failing before a start (no answer, not an RCP, missing caps): rcp incompatible",
          "[mode_fsm]")
{
    TEST_ASSERT_TRUE(mode_fsm_probe_fault_incompatible(MODE_FSM_BUILTIN_ID, ESP_ERR_TIMEOUT));
    TEST_ASSERT_TRUE(mode_fsm_probe_fault_incompatible(MODE_FSM_BUILTIN_ID, ESP_ERR_NOT_SUPPORTED));
    TEST_ASSERT_TRUE(mode_fsm_probe_fault_incompatible(MODE_FSM_BUILTIN_ID, ESP_ERR_INVALID_RESPONSE));
    TEST_ASSERT_TRUE(mode_fsm_probe_fault_incompatible(MODE_FSM_BUILTIN_ID, ESP_FAIL));
}

TEST_CASE("On the C6's channel going away during the probe: not an RCP verdict (the probe's text stays)",
          "[mode_fsm]")
{
    TEST_ASSERT_FALSE(mode_fsm_probe_fault_incompatible(MODE_FSM_BUILTIN_ID, ESP_ERR_INVALID_STATE));
}

TEST_CASE("On a dongle's probe failing, or a probe that passed: not rcp incompatible", "[mode_fsm]")
{
    TEST_ASSERT_FALSE(mode_fsm_probe_fault_incompatible(DST, ESP_ERR_TIMEOUT));
    TEST_ASSERT_FALSE(mode_fsm_probe_fault_incompatible(NULL, ESP_ERR_TIMEOUT));
    TEST_ASSERT_FALSE(mode_fsm_probe_fault_incompatible(MODE_FSM_BUILTIN_ID, ESP_OK));
}

/* ---- the retry of a failed start (V8) ---- */

TEST_CASE("On RADIO_FAULT with OpenThread stopped: the failed start is recorded for a retry", "[mode_fsm]")
{
    TEST_ASSERT_TRUE(mode_fsm_start_failed(BR_MODE_RADIO_FAULT, false));
}

TEST_CASE("On RADIO_FAULT with OpenThread still running, or any other mode: no retry recorded", "[mode_fsm]")
{
    TEST_ASSERT_FALSE(mode_fsm_start_failed(BR_MODE_RADIO_FAULT, true));
    TEST_ASSERT_FALSE(mode_fsm_start_failed(BR_MODE_BR_RUNNING, false));
    TEST_ASSERT_FALSE(mode_fsm_start_failed(BR_MODE_NO_RADIO, false));
    TEST_ASSERT_FALSE(mode_fsm_start_failed(BR_MODE_STARTING, false));
}

TEST_CASE("On a move whose start on the target and back on the source fail: a retry is recorded", "[mode_fsm]")
{
    char err[64];

    setup(BR_MODE_BR_RUNNING);
    fake.start_fail_on[0] = DST;
    fake.start_fail_on[1] = SRC;
    mode_fsm_move_otbr(&fsm, SRC, DST, err, sizeof(err));
    TEST_ASSERT_TRUE(mode_fsm_start_failed(mode_fsm_get_mode(&fsm), false));
}

TEST_CASE("On a move whose start on the target fails but back on the source works: no retry recorded", "[mode_fsm]")
{
    char err[64];

    setup(BR_MODE_BR_RUNNING);
    fake.start_fail_on[0] = DST;
    mode_fsm_move_otbr(&fsm, SRC, DST, err, sizeof(err));
    TEST_ASSERT_FALSE(mode_fsm_start_failed(mode_fsm_get_mode(&fsm), true));
}

/* ---- the board-mode table (design section 6): mode_fsm_mode_for ---- */

#define USB_ID "usb-303a-1001"

// Thread bound to a ready dongle, not started.
static mode_fsm_view_t view(void)
{
    return (mode_fsm_view_t) {
        .bound = true, .chan_state = RADIO_MUX_CS_READY, .kind = RADIO_MUX_FAULT_NONE, .work = RADIO_MUX_WORK_NONE,
        .chan_id = USB_ID, .chan_reason = "", .thread = BR_MODE_NO_RADIO, .thread_reason = "", .unplugged = false,
    };
}

static br_core_mode_t mode_for(const mode_fsm_view_t *v, char *reason)
{
    strcpy(reason, "stale");
    return mode_fsm_mode_for(v, reason, 96);
}

TEST_CASE("On Thread running on a ready channel: BR_RUNNING, no reason", "[mode_fsm]")
{
    mode_fsm_view_t v = view();
    char reason[96];

    v.thread = BR_MODE_BR_RUNNING;
    TEST_ASSERT_EQUAL(BR_MODE_BR_RUNNING, mode_for(&v, reason));
    TEST_ASSERT_EQUAL_STRING("", reason);
}

TEST_CASE("On Thread sniffing on a ready channel: SNIFFER", "[mode_fsm]")
{
    mode_fsm_view_t v = view();
    char reason[96];

    v.thread = BR_MODE_SNIFFER;
    TEST_ASSERT_EQUAL(BR_MODE_SNIFFER, mode_for(&v, reason));
    TEST_ASSERT_EQUAL_STRING("", reason);
}

TEST_CASE("On Thread starting or releasing, whatever its channel: STARTING", "[mode_fsm]")
{
    mode_fsm_view_t v = view();
    char reason[96];

    v.thread = BR_MODE_STARTING;
    TEST_ASSERT_EQUAL(BR_MODE_STARTING, mode_for(&v, reason));
    v.chan_state = RADIO_MUX_CS_ABSENT;
    TEST_ASSERT_EQUAL(BR_MODE_STARTING, mode_for(&v, reason));
    TEST_ASSERT_EQUAL_STRING("", reason);
}

TEST_CASE("On Thread bound and waiting for the C6's boot steps, push or ROM recovery: STARTING", "[mode_fsm]")
{
    const radio_mux_work_t works[] = {RADIO_MUX_WORK_BOOT_STEPS, RADIO_MUX_WORK_PUSH, RADIO_MUX_WORK_ROM_RECOVERY};
    mode_fsm_view_t v = view();
    char reason[96];

    v.chan_id = "builtin-c6";
    v.chan_state = RADIO_MUX_CS_REINIT;
    for (size_t i = 0; i < sizeof(works) / sizeof(works[0]); i++) {
        v.work = works[i];
        TEST_ASSERT_EQUAL(BR_MODE_STARTING, mode_for(&v, reason));
        TEST_ASSERT_EQUAL_STRING("", reason);
    }
}

TEST_CASE("On Thread bound and waiting for a dongle's firmware detection: NO_RADIO waiting for <id>", "[mode_fsm]")
{
    mode_fsm_view_t v = view();
    char reason[96];

    v.chan_state = RADIO_MUX_CS_REINIT;
    v.work = RADIO_MUX_WORK_DETECT;
    TEST_ASSERT_EQUAL(BR_MODE_NO_RADIO, mode_for(&v, reason));
    TEST_ASSERT_EQUAL_STRING("waiting for " USB_ID, reason);
}

TEST_CASE("On Thread bound to an absent dongle at boot: NO_RADIO waiting for <id>", "[mode_fsm]")
{
    mode_fsm_view_t v = view();
    char reason[96];

    v.chan_state = RADIO_MUX_CS_ABSENT;
    TEST_ASSERT_EQUAL(BR_MODE_NO_RADIO, mode_for(&v, reason));
    TEST_ASSERT_EQUAL_STRING("waiting for " USB_ID, reason);
}

TEST_CASE("On Thread bound to a dongle unplugged after it ran: NO_RADIO radio unplugged (<id>)", "[mode_fsm]")
{
    mode_fsm_view_t v = view();
    char reason[96];

    v.chan_state = RADIO_MUX_CS_ABSENT;
    v.unplugged = true;
    TEST_ASSERT_EQUAL(BR_MODE_NO_RADIO, mode_for(&v, reason));
    TEST_ASSERT_EQUAL_STRING("radio unplugged (" USB_ID ")", reason);
}

TEST_CASE("On Thread's channel in a link fault: NO_RADIO with the channel's reason and id", "[mode_fsm]")
{
    mode_fsm_view_t v = view();
    char reason[96];

    v.chan_state = RADIO_MUX_CS_FAULT;
    v.kind = RADIO_MUX_FAULT_LINK;
    v.chan_reason = "no answer after 3 resets";
    TEST_ASSERT_EQUAL(BR_MODE_NO_RADIO, mode_for(&v, reason));
    TEST_ASSERT_EQUAL_STRING("no answer after 3 resets (" USB_ID ")", reason);
}

TEST_CASE("On Thread's channel in a firmware fault: RADIO_FAULT with the channel's reason", "[mode_fsm]")
{
    mode_fsm_view_t v = view();
    char reason[96];

    v.chan_id = "builtin-c6";
    v.chan_state = RADIO_MUX_CS_FAULT;
    v.kind = RADIO_MUX_FAULT_FIRMWARE;
    v.chan_reason = "no rcp firmware";
    TEST_ASSERT_EQUAL(BR_MODE_RADIO_FAULT, mode_for(&v, reason));
    TEST_ASSERT_EQUAL_STRING("no rcp firmware", reason);
    v.thread = BR_MODE_RADIO_FAULT; // after its LOST (firmware) as well
    TEST_ASSERT_EQUAL(BR_MODE_RADIO_FAULT, mode_for(&v, reason));
    TEST_ASSERT_EQUAL_STRING("no rcp firmware", reason);
}

TEST_CASE("On Thread in fault on a ready channel: RADIO_FAULT with the start's reason", "[mode_fsm]")
{
    mode_fsm_view_t v = view();
    char reason[96];

    v.chan_id = "builtin-c6";
    v.thread = BR_MODE_RADIO_FAULT;
    v.thread_reason = MODE_FSM_FAULT_INCOMPATIBLE;
    TEST_ASSERT_EQUAL(BR_MODE_RADIO_FAULT, mode_for(&v, reason));
    TEST_ASSERT_EQUAL_STRING("rcp incompatible", reason);
    v.thread_reason = "an earlier OpenThread start was abandoned: reboot";
    TEST_ASSERT_EQUAL(BR_MODE_RADIO_FAULT, mode_for(&v, reason));
    TEST_ASSERT_EQUAL_STRING("an earlier OpenThread start was abandoned: reboot", reason);
}

TEST_CASE("On Thread bound to a ready channel whose firmware does not fit: NO_RADIO with the fit reason", "[mode_fsm]")
{
    mode_fsm_view_t v = view();
    char reason[96];

    v.thread_reason = "detected znp firmware does not fit otbr";
    TEST_ASSERT_EQUAL(BR_MODE_NO_RADIO, mode_for(&v, reason));
    TEST_ASSERT_EQUAL_STRING("detected znp firmware does not fit otbr", reason);
}

TEST_CASE("On no channel stored in otbr: NO_RADIO no channel in otbr", "[mode_fsm]")
{
    mode_fsm_view_t v = view();
    char reason[96];

    v.bound = false;
    v.chan_id = NULL;
    TEST_ASSERT_EQUAL(BR_MODE_NO_RADIO, mode_for(&v, reason));
    TEST_ASSERT_EQUAL_STRING("no channel in otbr", reason);
}

/* ---- Thread's answer to a reinit request: mode_fsm_reinit_verdict ---- */

TEST_CASE("On a reinit request while BR_RUNNING: accepts (after the sessions drain)", "[mode_fsm]")
{
    TEST_ASSERT_EQUAL(ESP_OK, mode_fsm_reinit_verdict(BR_MODE_BR_RUNNING));
    TEST_ASSERT_EQUAL(ESP_OK, mode_fsm_reinit_verdict(BR_MODE_RADIO_FAULT));
    TEST_ASSERT_EQUAL(ESP_OK, mode_fsm_reinit_verdict(BR_MODE_NO_RADIO));
}

TEST_CASE("On a reinit request in SNIFFER: refuses with ESP_ERR_INVALID_STATE", "[mode_fsm]")
{
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, mode_fsm_reinit_verdict(BR_MODE_SNIFFER));
}

TEST_CASE("On a reinit request in STARTING: refuses with ESP_ERR_INVALID_STATE", "[mode_fsm]")
{
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, mode_fsm_reinit_verdict(BR_MODE_STARTING));
}

/* ---- inputs: mode_fsm_input_newer ---- */

TEST_CASE("On an input newer than the last one handled: acted on", "[mode_fsm]")
{
    TEST_ASSERT_TRUE(mode_fsm_input_newer(0, 1));
    TEST_ASSERT_TRUE(mode_fsm_input_newer(2, 9));
    TEST_ASSERT_TRUE(mode_fsm_input_newer(UINT32_MAX, 0));
}

TEST_CASE("On an input older than the latest, or the one handled: is ignored", "[mode_fsm]")
{
    TEST_ASSERT_FALSE(mode_fsm_input_newer(9, 9));
    TEST_ASSERT_FALSE(mode_fsm_input_newer(9, 2));
}

/* ---- inputs: mode_fsm_inbox (the binding input and the pending request kept apart) ---- */

static void put(mode_fsm_inbox_t *box, radio_mux_input_type_t type, uint32_t seq)
{
    const radio_mux_input_t in = {.type = type, .seq = seq};

    mode_fsm_inbox_put(box, &in);
}

TEST_CASE("On a REINIT after an unhandled BIND: both are taken fresh (the start still happens)", "[mode_fsm]")
{
    mode_fsm_inbox_t box = {0};
    radio_mux_input_t in;

    put(&box, RADIO_MUX_IN_BIND, 1);
    put(&box, RADIO_MUX_IN_REINIT, 2);
    TEST_ASSERT_TRUE(mode_fsm_inbox_take_req(&box, &in));
    TEST_ASSERT_EQUAL(RADIO_MUX_IN_REINIT, in.type);
    TEST_ASSERT_EQUAL_UINT32(2, in.seq);
    TEST_ASSERT_TRUE(mode_fsm_inbox_take_bind(&box, &in));
    TEST_ASSERT_EQUAL(RADIO_MUX_IN_BIND, in.type);
    TEST_ASSERT_EQUAL_UINT32(1, in.seq);
}

TEST_CASE("On a binding input: replaces the last binding input, the request kept", "[mode_fsm]")
{
    mode_fsm_inbox_t box = {0};
    radio_mux_input_t in;

    put(&box, RADIO_MUX_IN_REINIT, 1);
    put(&box, RADIO_MUX_IN_BIND, 2);
    put(&box, RADIO_MUX_IN_LOST, 3);
    TEST_ASSERT_TRUE(mode_fsm_inbox_take_bind(&box, &in));
    TEST_ASSERT_EQUAL(RADIO_MUX_IN_LOST, in.type);
    TEST_ASSERT_EQUAL_UINT32(3, in.seq);
    TEST_ASSERT_TRUE(mode_fsm_inbox_take_req(&box, &in));
    TEST_ASSERT_EQUAL(RADIO_MUX_IN_REINIT, in.type);
    TEST_ASSERT_EQUAL_UINT32(1, in.seq);
}

TEST_CASE("On an input taken: is not fresh again", "[mode_fsm]")
{
    mode_fsm_inbox_t box = {0};
    radio_mux_input_t in;

    TEST_ASSERT_FALSE(mode_fsm_inbox_take_bind(&box, &in));
    TEST_ASSERT_FALSE(mode_fsm_inbox_take_req(&box, &in));
    put(&box, RADIO_MUX_IN_BIND, 1);
    TEST_ASSERT_TRUE(mode_fsm_inbox_take_bind(&box, &in));
    TEST_ASSERT_FALSE(mode_fsm_inbox_take_bind(&box, &in));
    put(&box, RADIO_MUX_IN_REINIT, 2);
    TEST_ASSERT_TRUE(mode_fsm_inbox_take_req(&box, &in));
    TEST_ASSERT_FALSE(mode_fsm_inbox_take_req(&box, &in));
}

TEST_CASE("On letting go for a request newer than the binding input: the binding input is not acted on",
          "[mode_fsm]")
{
    mode_fsm_inbox_t box = {0};
    radio_mux_input_t in;

    put(&box, RADIO_MUX_IN_BACK, 1);
    put(&box, RADIO_MUX_IN_REINIT, 2);
    TEST_ASSERT_TRUE(mode_fsm_inbox_take_req(&box, &in));
    mode_fsm_inbox_let_go(&box, in.seq); // the maintenance's end binds it back
    TEST_ASSERT_FALSE(mode_fsm_inbox_take_bind(&box, &in));
}

TEST_CASE("On a binding input newer than the request let go for: is still acted on", "[mode_fsm]")
{
    mode_fsm_inbox_t box = {0};
    radio_mux_input_t in;

    put(&box, RADIO_MUX_IN_REINIT, 1);
    put(&box, RADIO_MUX_IN_BACK, 2);
    TEST_ASSERT_TRUE(mode_fsm_inbox_take_req(&box, &in));
    mode_fsm_inbox_let_go(&box, in.seq);
    TEST_ASSERT_TRUE(mode_fsm_inbox_take_bind(&box, &in));
    TEST_ASSERT_EQUAL(RADIO_MUX_IN_BACK, in.type);
}

TEST_CASE("On letting go for a request older than the binding input taken: that input stays taken", "[mode_fsm]")
{
    mode_fsm_inbox_t box = {0};
    radio_mux_input_t in;

    put(&box, RADIO_MUX_IN_BIND, 3);
    TEST_ASSERT_TRUE(mode_fsm_inbox_take_bind(&box, &in));
    mode_fsm_inbox_let_go(&box, 2);
    TEST_ASSERT_FALSE(mode_fsm_inbox_take_bind(&box, &in));
    put(&box, RADIO_MUX_IN_BACK, 4);
    TEST_ASSERT_TRUE(mode_fsm_inbox_take_bind(&box, &in));
}

TEST_CASE("On an input not taken in either slot: pending until taken", "[mode_fsm]")
{
    mode_fsm_inbox_t box = {0};
    radio_mux_input_t in;

    TEST_ASSERT_FALSE(mode_fsm_inbox_pending(&box));
    put(&box, RADIO_MUX_IN_BIND, 1);
    TEST_ASSERT_TRUE(mode_fsm_inbox_pending(&box));
    mode_fsm_inbox_take_bind(&box, &in);
    TEST_ASSERT_FALSE(mode_fsm_inbox_pending(&box));
    put(&box, RADIO_MUX_IN_REINIT, 2);
    TEST_ASSERT_TRUE(mode_fsm_inbox_pending(&box));
    mode_fsm_inbox_take_req(&box, &in);
    TEST_ASSERT_FALSE(mode_fsm_inbox_pending(&box));
}

TEST_CASE("On a binding input acted on elsewhere (a move's commit): it is taken, the request still fresh",
          "[mode_fsm]")
{
    mode_fsm_inbox_t box = {0};
    radio_mux_input_t in;

    put(&box, RADIO_MUX_IN_REINIT, 1);
    put(&box, RADIO_MUX_IN_BIND, 2);
    mode_fsm_inbox_handled(&box);
    TEST_ASSERT_FALSE(mode_fsm_inbox_take_bind(&box, &in));
    TEST_ASSERT_TRUE(mode_fsm_inbox_take_req(&box, &in));
}

/* ---- the mode_changed op (Thread's state reported to the orchestrator) ---- */

TEST_CASE("On every mode change: the mode_changed op hears the new mode", "[mode_fsm]")
{
    setup(BR_MODE_STARTING);
    TEST_ASSERT_TRUE(mode_fsm_set_started(&fsm));
    mode_fsm_set_fault(&fsm);
    mode_fsm_on_channel_gone(&fsm);
    TEST_ASSERT_EQUAL(4, fake.heard_count); /* BR_RUNNING, RADIO_FAULT, NO_RADIO (before and after the teardown) */
    TEST_ASSERT_EQUAL(BR_MODE_BR_RUNNING, fake.heard[0]);
    TEST_ASSERT_EQUAL(BR_MODE_RADIO_FAULT, fake.heard[1]);
    TEST_ASSERT_EQUAL(BR_MODE_NO_RADIO, fake.heard[2]);
    TEST_ASSERT_EQUAL(BR_MODE_NO_RADIO, fake.heard[3]);
}

/* ---- REINIT: mode_fsm_on_reinit ---- */

TEST_CASE("On a reinit while BR_RUNNING: OpenThread stops after its drain, STARTING, accepted", "[mode_fsm]")
{
    setup(BR_MODE_BR_RUNNING);
    TEST_ASSERT_EQUAL(ESP_OK, mode_fsm_on_reinit(&fsm));
    TEST_ASSERT_EQUAL(1, count_of("ot_stop"));
    TEST_ASSERT_EQUAL(BR_MODE_STARTING, fake.mode_in_stop); /* the guards refuse new sessions during the drain */
    TEST_ASSERT_EQUAL(BR_MODE_STARTING, mode_fsm_get_mode(&fsm));
    TEST_ASSERT_EQUAL(0, count_of("ot_start"));
}

TEST_CASE("On a reinit in SNIFFER or STARTING: refused with ESP_ERR_INVALID_STATE, nothing stopped", "[mode_fsm]")
{
    setup(BR_MODE_SNIFFER);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, mode_fsm_on_reinit(&fsm));
    TEST_ASSERT_EQUAL(BR_MODE_SNIFFER, mode_fsm_get_mode(&fsm));
    setup(BR_MODE_STARTING);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, mode_fsm_on_reinit(&fsm));
    TEST_ASSERT_EQUAL(BR_MODE_STARTING, mode_fsm_get_mode(&fsm));
    TEST_ASSERT_EQUAL(0, count_of("ot_stop"));
}

TEST_CASE("On a reinit whose sessions do not end: refused with a drain timeout, the mode back", "[mode_fsm]")
{
    setup(BR_MODE_BR_RUNNING);
    fake.fail[OP_OT_STOP] = ESP_ERR_TIMEOUT;
    TEST_ASSERT_EQUAL(MODE_FSM_ERR_DRAIN_TIMEOUT, mode_fsm_on_reinit(&fsm));
    TEST_ASSERT_EQUAL(BR_MODE_BR_RUNNING, mode_fsm_get_mode(&fsm)); /* never RADIO_FAULT */
}

TEST_CASE("On a reinit with OpenThread not running (RADIO_FAULT, NO_RADIO): accepted, STARTING", "[mode_fsm]")
{
    setup(BR_MODE_RADIO_FAULT);
    TEST_ASSERT_EQUAL(ESP_OK, mode_fsm_on_reinit(&fsm));
    TEST_ASSERT_EQUAL(BR_MODE_STARTING, mode_fsm_get_mode(&fsm));
    setup(BR_MODE_NO_RADIO);
    TEST_ASSERT_EQUAL(ESP_OK, mode_fsm_on_reinit(&fsm));
    TEST_ASSERT_EQUAL(BR_MODE_STARTING, mode_fsm_get_mode(&fsm));
}
