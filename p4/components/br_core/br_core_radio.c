// OpenThread on a radio mux channel (spec section 3, OTBR mode): the Thread instance of the orchestrator, the mux
// operations of MuxSpinelInterface, the RCP probe through the channel, OpenThread's start and stop on it, and the
// br_radio task that acts on the orchestrator's inputs (bind, unbind, lost, back, reinit) and runs moves. The OpenThread state machine's other operations, the boot and the
// C6 jobs are in br_core.c.
#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>

#include "board_cores.h"
#include "br_core.h"
#include "br_core_priv.h"
#include "esp_br_web.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_openthread_lock.h"
#include "esp_openthread_mux.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "openthread/platform/radio.h"
#include "otbr_probe.h"
#include "radio_mux.h"

static const char *TAG = "br_core";

// The radio task (br_radio): OpenThread's start and stop on the OTBR channel, moves, the channel's GONE and return.
#define RADIO_TASK_NAME "br_radio"
#define RADIO_TASK_STACK 6144
#define RADIO_TASK_PRIORITY 5
#define MOVE_PROBE_WAIT_MS 10000 // the PUT that moves Thread waits this long for the target's probe
#define PROBE_RESET_WAIT_MS 2000 // the RCP's reset notification after a spinel reset
#define PROBE_ANSWER_WAIT_MS 500 // each property answer
#define PROBE_POLL_SLICE_MS 50
#define FAULT_NO_CHANNEL_START "OpenThread did not start on its channel"
#define FAULT_C6_INCOMPATIBLE MODE_FSM_FAULT_INCOMPATIBLE
// After the OTBR channel was lost (unplugged, failed), the teardown waits for the open OpenThread sessions in rounds of
// this long and never deletes the OpenThread lock under one: esp_openthread_deinit frees it, and FreeRTOS does not check
// for waiters. Longer than httpd's 15 s socket I/O timeout, so a REST handler stuck writing to a slow client ends within
// one round; the board is NO_RADIO meanwhile (new sessions refused) and the radio calls of the open ones are answered
// at once (mux_spinel_interface.cpp).
#define LOST_DRAIN_MS 20000
_Static_assert(BR_CORE_CHANNEL_ID_MAX == RADIO_MUX_ID_MAX, "br_core_status_t.otbr_channel holds a channel id");
_Static_assert(OTBR_RADIO_CAP_ACK_TIMEOUT == OT_RADIO_CAPS_ACK_TIMEOUT &&
               OTBR_RADIO_CAP_TRANSMIT_RETRIES == OT_RADIO_CAPS_TRANSMIT_RETRIES &&
               OTBR_RADIO_CAP_CSMA_BACKOFF == OT_RADIO_CAPS_CSMA_BACKOFF &&
               OTBR_RADIO_CAP_TRANSMIT_SEC == OT_RADIO_CAPS_TRANSMIT_SEC &&
               OTBR_RADIO_CAP_TRANSMIT_TIMING == OT_RADIO_CAPS_TRANSMIT_TIMING, "otbr_probe.h radio caps");

// The channel OpenThread runs on (the context of the mux operations). `gone`: the channel was detached or posted GONE;
// from then on the interface refuses requests until OpenThread stops. Set on every start.
typedef struct {
    radio_mux_chan_t *_Atomic ch;
    atomic_bool gone;
} otbr_link_t;
static otbr_link_t s_link;
// The channel Thread is bound to now: set by BIND and BACK, cleared by UNBIND and LOST (the mux owner task, on_input).
static radio_mux_chan_t *_Atomic s_attached;
// The handle Thread's state is reported through (radio_mux_report): set by BIND and BACK, cleared by UNBIND only (a lost
// channel still shows why).
static radio_mux_chan_t *_Atomic s_report_ch;
// The orchestrator's latest binding input and latest request, each with the seq of the last one acted on (by br_radio,
// or by a move's commit that acted on the binding input itself), under s_in_lock.
static portMUX_TYPE s_in_lock = portMUX_INITIALIZER_UNLOCKED;
static mode_fsm_inbox_t s_inbox;
static TaskHandle_t _Atomic s_radio_task;
static const esp_openthread_mux_ops_t s_mux_ops;
static bool s_move_probe;   // br_radio: the next probe is a move's (its verdict answers the waiting PUT)
static bool s_start_failed; // br_radio: the last start on the attached channel failed (RADIO_FAULT from it)
static atomic_bool s_retry;   // br_core_retry_otbr: try the failed start on the attached channel again
static void (*_Atomic s_move_done)(void); // br_core_set_move_done_cb
static char s_no_radio_reason[96]; // br_radio: why the mode is NO_RADIO
static bool s_lost;                // br_radio: OpenThread's channel was lost, not unbound by a deliberate change
// br_radio runs a channel operation (OpenThread torn down after GONE, or started after an attach): a move, start or
// stop asked for meanwhile is refused at once (br_core_move_otbr) instead of waiting for its probe.
static atomic_bool s_radio_busy;
// What br_radio waits for inside that operation, and since when (ticks): the refusal says it, and a refusal after
// RADIO_BUSY_WARN_MS logs it, so a wait that never ends shows on the board instead of a bare 409.
#define RADIO_BUSY_WARN_MS 30000
static const char *_Atomic s_radio_step = "";
static atomic_uint s_radio_busy_since;
// The last refused stop of OpenThread (its sessions did not end): the channel, which runs otbr on, and why. Kept until
// the next accepted move, start or stop, or channel operation; under s_refusal_lock (device_api reads it).
static portMUX_TYPE s_refusal_lock = portMUX_INITIALIZER_UNLOCKED;
static char s_refused_id[RADIO_MUX_ID_MAX];
static char s_refused_reason[RADIO_MUX_REASON_MAX];
// Owned by br_radio, the task that starts and stops OpenThread:
static radio_mux_chan_t *s_ot_chan; // the channel (handle) OpenThread was started on, NULL when it is stopped
static radio_mux_chan_t *s_lease;   // a move's target, leased from the mux until the move ends
static radio_mux_fw_record_t s_probe_fw; // what the last probe read
static char s_probe_err[64];        // why the last probe failed: the fault reason a failed start shows
// A move of OpenThread: the caller of br_core_move_otbr waits for the probe's verdict, br_radio runs the move.
enum { MOVE_IDLE, MOVE_WAITING, MOVE_SIGNALLED, MOVE_ABANDONED };
static struct {
    atomic_bool running;   // from the request until br_radio finished it
    atomic_bool queued;    // for br_radio: run it
    atomic_int state;
    SemaphoreHandle_t probed;
    char from[RADIO_MUX_ID_MAX], to[RADIO_MUX_ID_MAX];
    esp_err_t result;
    char reason[RADIO_MUX_REASON_MAX];
} s_move;

// ---- OpenThread on a radio mux channel: the otbr consumer, the probe, start, stop, moves (spec section 3) ----

// The blocking step br_radio is in (a string literal).
static void radio_step(const char *step)
{
    atomic_store(&s_radio_step, step);
}

static void radio_busy_begin(const char *step)
{
    radio_step(step);
    atomic_store(&s_radio_busy_since, (unsigned)xTaskGetTickCount());
    atomic_store(&s_radio_busy, true);
}

static void radio_busy_end(void)
{
    atomic_store(&s_radio_busy, false);
    radio_step("");
}

void core_radio_notify(void)
{
    TaskHandle_t task = atomic_load(&s_radio_task);

    if (task) {
        xTaskNotifyGive(task);
    }
}

// The mux operations of MuxSpinelInterface (esp_openthread_mux.h), on the OpenThread task under the OpenThread lock.
// After `gone` nothing goes to the channel or comes from it any more, whatever the mux would still allow: the mux's
// generations refuse a handle of an earlier attach, and this belt covers the time between GONE and the next attach.
// What is still queued then is drained, not delivered: the slots go back to the pool and the last, empty poll clears
// the eventfd (MuxSpinelInterface keeps the eventfd out of the main loop's select after GONE).
static esp_err_t mux_tx(void *ctx, const uint8_t *f, size_t n)
{
    const otbr_link_t *link = ctx;
    radio_mux_chan_t *ch = atomic_load(&link->ch);

    return ch && !atomic_load(&link->gone) ? radio_mux_tx(ch, f, n) : ESP_ERR_INVALID_STATE;
}

static bool mux_poll(void *ctx, uint8_t **slot, size_t *len)
{
    otbr_link_t *link = ctx;
    radio_mux_chan_t *ch = atomic_load(&link->ch);
    radio_mux_ev_t ev;

    while (ch && radio_mux_poll(ch, &ev)) {
        if (atomic_load(&link->gone)) {
            if (ev.type == RADIO_MUX_EV_RX_FRAME) {
                radio_mux_release(ch, ev.slot);
            }
            continue;
        }
        switch (ev.type) {
        case RADIO_MUX_EV_RX_FRAME:
            *slot = ev.slot;
            *len = ev.len;
            return true;
        case RADIO_MUX_EV_RESET:
            *slot = NULL;
            *len = 0;
            return true;
        case RADIO_MUX_EV_GONE:
            atomic_store(&link->gone, true);
            core_radio_notify();
            break;
        }
    }
    return false;
}

static void mux_release(void *ctx, uint8_t *slot)
{
    radio_mux_release(atomic_load(&((otbr_link_t *)ctx)->ch), slot);
}

static int mux_eventfd(void *ctx)
{
    radio_mux_chan_t *ch = atomic_load(&((otbr_link_t *)ctx)->ch);

    return ch ? radio_mux_eventfd(ch) : -1;
}

static esp_err_t mux_reset(void *ctx)
{
    radio_mux_chan_t *ch = atomic_load(&((otbr_link_t *)ctx)->ch);

    return ch ? radio_mux_reset(ch) : ESP_ERR_INVALID_STATE;
}

static bool mux_gone(void *ctx)
{
    const otbr_link_t *link = ctx;

    return atomic_load(&link->gone) || !atomic_load(&link->ch);
}

static const esp_openthread_mux_ops_t s_mux_ops = {
    .tx = mux_tx,
    .poll = mux_poll,
    .release = mux_release,
    .eventfd = mux_eventfd,
    .reset = mux_reset,
    .gone = mux_gone,
};

// The Thread instance's input (radio mux owner task): the bound channel noted at once (a C6 job's restart reads it),
// the input kept as the latest of its slot (a REINIT never replaces a binding input), br_radio woken to act on it.
// Never blocks.
static void on_input(const radio_mux_input_t *in, void *ctx)
{
    switch (in->type) {
    case RADIO_MUX_IN_BIND:
    case RADIO_MUX_IN_BACK:
        atomic_store(&s_attached, in->ch);
        atomic_store(&s_report_ch, in->ch);
        break;
    case RADIO_MUX_IN_UNBIND:
    case RADIO_MUX_IN_LOST: {
        radio_mux_chan_t *expected = in->ch;

        atomic_compare_exchange_strong(&s_attached, &expected, NULL);
        if (in->ch && atomic_load(&s_link.ch) == in->ch) {
            atomic_store(&s_link.gone, true); // the interface refuses requests from now on (no recovery, no DieNow)
        }
        if (in->type == RADIO_MUX_IN_UNBIND) {
            atomic_store(&s_report_ch, NULL);
        }
        break;
    }
    default:
        break;
    }
    taskENTER_CRITICAL(&s_in_lock);
    mode_fsm_inbox_put(&s_inbox, in);
    taskEXIT_CRITICAL(&s_in_lock);
    core_radio_notify();
}

// The latest request (want_req) or binding input into *in; true when it was not acted on yet (it is from now on).
static bool take_input(bool want_req, radio_mux_input_t *in)
{
    taskENTER_CRITICAL(&s_in_lock);
    const bool fresh = want_req ? mode_fsm_inbox_take_req(&s_inbox, in) : mode_fsm_inbox_take_bind(&s_inbox, in);
    taskEXIT_CRITICAL(&s_in_lock);
    return fresh;
}

// A move's commit acted on the latest binding input itself (as br_radio would have).
static void input_handled(void)
{
    taskENTER_CRITICAL(&s_in_lock);
    mode_fsm_inbox_handled(&s_inbox);
    taskEXIT_CRITICAL(&s_in_lock);
}

// Thread's state names in the orchestrator's read API (design section 4.2).
static const char *thread_state(br_core_mode_t mode)
{
    switch (mode) {
    case BR_MODE_STARTING:
        return "starting";
    case BR_MODE_BR_RUNNING:
        return "running";
    case BR_MODE_SNIFFER:
        return "sniffer";
    case BR_MODE_RADIO_FAULT:
        return "fault";
    default:
        return "no_radio";
    }
}

void core_radio_mode_changed(br_core_mode_t mode)
{
    radio_mux_report(atomic_load(&s_report_ch), thread_state(mode), NULL); // the reason follows from br_radio
    core_radio_notify();
}

// br_radio: Thread's state and the board's reason to the orchestrator.
static void report_state(void)
{
    char reason[sizeof(g_core.fault_reason)];

    xSemaphoreTake(g_core.mutex, portMAX_DELAY);
    strlcpy(reason, g_core.fault_reason, sizeof(reason));
    xSemaphoreGive(g_core.mutex);
    radio_mux_report(atomic_load(&s_report_ch), thread_state(br_core_get_mode()), reason);
}

static void wait_readable(int fd, int64_t us)
{
    if (fd < 0) {
        vTaskDelay(pdMS_TO_TICKS(PROBE_POLL_SLICE_MS));
        return;
    }
    fd_set read_fds;
    struct timeval timeout = {.tv_sec = (time_t)(us / 1000000), .tv_usec = (suseconds_t)(us % 1000000)};

    FD_ZERO(&read_fds);
    FD_SET(fd, &read_fds);
    select(fd + 1, &read_fds, NULL, NULL, &timeout);
}

// Waits up to `ms` for the RCP's reset notification (`tid` 0, `key` ignored) or the answer to request `tid` about `key`;
// other frames are given back. The answer's value goes to `value` (cut to *len, *len set). ESP_ERR_INVALID_STATE on
// GONE, ESP_ERR_TIMEOUT.
static esp_err_t probe_wait(radio_mux_chan_t *ch, uint8_t tid, uint32_t key, uint8_t *value, size_t *len, uint32_t ms)
{
    const int64_t deadline = esp_timer_get_time() + (int64_t)ms * 1000;
    radio_mux_ev_t ev;

    for (;;) {
        while (radio_mux_poll(ch, &ev)) {
            if (ev.type == RADIO_MUX_EV_GONE) {
                return ESP_ERR_INVALID_STATE;
            }
            if (ev.type != RADIO_MUX_EV_RX_FRAME) {
                continue;
            }
            otbr_spinel_msg_t m;
            const bool hit = otbr_probe_parse(ev.slot, ev.len, &m) &&
                             (tid == 0 ? otbr_probe_is_reset(&m) : otbr_probe_is_answer(&m, tid, key));

            if (hit && value) {
                *len = m.value_len < *len ? m.value_len : *len;
                memcpy(value, m.value, *len);
            }
            radio_mux_release(ch, ev.slot);
            if (hit) {
                return ESP_OK;
            }
        }
        const int64_t left = deadline - esp_timer_get_time();

        if (left <= 0) {
            return ESP_ERR_TIMEOUT;
        }
        wait_readable(radio_mux_eventfd(ch), left < PROBE_POLL_SLICE_MS * 1000 ? left : PROBE_POLL_SLICE_MS * 1000);
    }
}

// One property: GET with `tid`, its value into `value` (*len in: room, out: length).
static esp_err_t probe_get(radio_mux_chan_t *ch, uint8_t tid, uint32_t key, uint8_t *value, size_t *len)
{
    uint8_t frame[OTBR_PROBE_FRAME_MAX];
    const esp_err_t err = radio_mux_tx(ch, frame, otbr_probe_get_frame(tid, key, frame));

    return err == ESP_OK ? probe_wait(ch, tid, key, value, len, PROBE_ANSWER_WAIT_MS) : err;
}

// The probe before OpenThread starts on a channel (spec: Start): spinel reset, PROP_NCP_VERSION, the capabilities (an
// RCP, and the radio caps OpenThread requires). An incompatible or wrong-baud radio fails here, with the reason in
// `err`, never inside esp_openthread_init.
static esp_err_t probe_rcp(radio_mux_chan_t *ch, radio_mux_fw_record_t *fw, char *err, size_t cap)
{
    uint8_t frame[OTBR_PROBE_FRAME_MAX];
    uint8_t value[RADIO_MUX_DETECT_RX_MAX];
    size_t len;
    uint32_t radio_caps = 0;
    radio_mux_ev_t ev;
    esp_err_t ret;

    while (radio_mux_poll(ch, &ev)) { // what came before the probe
        if (ev.type == RADIO_MUX_EV_GONE) {
            snprintf(err, cap, "the radio channel went away");
            return ESP_ERR_INVALID_STATE;
        }
        if (ev.type == RADIO_MUX_EV_RX_FRAME) {
            radio_mux_release(ch, ev.slot);
        }
    }
    // A C6 that is still booting may not take the reset; it announces its own start then, which counts as well.
    ret = radio_mux_tx(ch, frame, otbr_probe_reset_frame(frame));
    if (ret != ESP_ERR_INVALID_STATE) {
        ret = probe_wait(ch, 0, 0, NULL, NULL, PROBE_RESET_WAIT_MS);
    }
    if (ret != ESP_OK) {
        snprintf(err, cap, "%s", ret == ESP_ERR_INVALID_STATE ? "the radio channel went away" :
                 "no spinel answer to a reset (no RCP firmware, or a wrong baud)");
        return ret;
    }
    len = sizeof(value) - 1;
    ret = probe_get(ch, 1, OTBR_SPINEL_PROP_NCP_VERSION, value, &len);
    if (ret != ESP_OK) {
        snprintf(err, cap, "no answer to PROP_NCP_VERSION");
        return ret;
    }
    value[len] = '\0';
    memset(fw, 0, sizeof(*fw));
    fw->type = RADIO_MUX_FW_SPINEL;
    radio_mux_detect_parse_spinel_version((const char *)value, fw->version, sizeof(fw->version), fw->chip,
                                          sizeof(fw->chip));
    len = sizeof(value);
    ret = probe_get(ch, 2, OTBR_SPINEL_PROP_CAPS, value, &len);
    if (ret != ESP_OK || !otbr_probe_has_cap(value, len, OTBR_SPINEL_CAP_CONFIG_RADIO)) {
        snprintf(err, cap, "%s", ret != ESP_OK ? "no answer to PROP_CAPS" : "not an RCP (no radio configuration cap)");
        return ret != ESP_OK ? ret : ESP_ERR_NOT_SUPPORTED;
    }
    len = sizeof(value);
    ret = probe_get(ch, 3, OTBR_SPINEL_PROP_RADIO_CAPS, value, &len);
    if (ret != ESP_OK || !otbr_probe_radio_caps(value, len, &radio_caps)) {
        snprintf(err, cap, "no answer to PROP_RADIO_CAPS");
        return ret != ESP_OK ? ret : ESP_ERR_INVALID_RESPONSE;
    }
    if (otbr_probe_missing_caps(radio_caps, err, cap) != 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    ESP_LOGI(TAG, "%s: RCP %s (%s), radio caps 0x%" PRIx32, radio_mux_chan_id(ch), fw->version, fw->chip, radio_caps);
    return ESP_OK;
}

// The otbr channel `id` as br_radio holds it: the move's lease, or the attached channel.
static radio_mux_chan_t *channel_for(const char *id)
{
    radio_mux_chan_t *ch = atomic_load(&s_attached);

    if (s_lease && strcmp(radio_mux_chan_id(s_lease), id) == 0) {
        return s_lease;
    }
    return ch && strcmp(radio_mux_chan_id(ch), id) == 0 ? ch : NULL;
}

// The probe's verdict to the PUT that waits for it (br_core_move_otbr). false: that PUT gave up, the move stops.
static bool move_signal(esp_err_t result, const char *reason)
{
    int waiting = MOVE_WAITING;

    s_move.result = result;
    snprintf(s_move.reason, sizeof(s_move.reason), "%s", reason ? reason : "");
    if (atomic_compare_exchange_strong(&s_move.state, &waiting, MOVE_SIGNALLED)) {
        xSemaphoreGive(s_move.probed);
        return true;
    }
    return waiting != MOVE_ABANDONED;
}

esp_err_t core_radio_probe_channel(const char *id, char *err, size_t cap)
{
    radio_mux_chan_t *ch = id[0] ? channel_for(id) : NULL;
    radio_mux_fw_record_t fw;
    esp_err_t ret = ESP_OK;
    const bool move = s_move_probe; // a move's probe refuses and changes nothing; another's failure is a fault

    if (s_move_probe && atomic_load(&s_move.state) == MOVE_ABANDONED) {
        s_move_probe = false;
        snprintf(err, cap, "the request gave up waiting for the probe");
        return ESP_ERR_TIMEOUT; // nothing leased, nothing probed
    }
    if (id[0] == '\0') { // a stop of OpenThread: no target to probe; accepted
        if (s_move_probe) {
            s_move_probe = false;
            if (!move_signal(ESP_OK, "")) {
                snprintf(err, cap, "the request gave up waiting for the stop to be accepted");
                return ESP_ERR_TIMEOUT;
            }
        }
        return ESP_OK;
    }
    if (!ch) { // not the attached channel: a move's target, taken from the mux until the move ends
        ret = radio_mux_lease(id, &s_lease, err, cap);
        ch = ret == ESP_OK ? s_lease : NULL;
    }
    if (ret == ESP_OK) {
        ret = probe_rcp(ch, &fw, err, cap);
    }
    if (ret == ESP_OK) {
        s_probe_fw = fw;
        if (ch != s_lease) {
            radio_mux_note_firmware(ch, &fw); // a lease's record is noted once the move stored it
        }
    } else {
        strlcpy(s_probe_err, err, sizeof(s_probe_err));
        if (!move && mode_fsm_probe_fault_incompatible(id, ret)) {
            // The C6's RCP did not answer as one: "rcp incompatible" with rcp.compatible false, as when OpenThread
            // rejects it (the cure is the same, a push); the probe's text stays in the log.
            ESP_LOGW(TAG, "%s: %s: the RCP counts as incompatible", id, err);
            strlcpy(s_probe_err, FAULT_C6_INCOMPATIBLE, sizeof(s_probe_err));
            core_set_c6_compatible(false);
        }
    }
    if (s_move_probe) {
        s_move_probe = false;
        if (!move_signal(ret == ESP_OK ? ESP_OK : ESP_ERR_INVALID_RESPONSE, err) && ret == ESP_OK) {
            snprintf(err, cap, "the request gave up waiting for the probe");
            return ESP_ERR_TIMEOUT;
        }
    }
    return ret;
}

// The drain of every stop: no new OpenThread session and the Thread REST flag off (under the OpenThread lock, where
// the session users re-check ot_ready), then the open sessions waited for. Up to SESSION_DRAIN_MS: false on a timeout,
// with ot_ready back (nothing stopped). With `must_stop` (nothing to go back to: the channel was lost, or the start
// failed) as long as it takes, a log per round: the teardown never deletes the lock and the instance under a session.
static bool close_sessions(bool must_stop, const char *why)
{
    if (core_ot_ready_now()) {
        esp_openthread_lock_acquire(portMAX_DELAY);
        esp_br_web_set_openthread_ready(false);
        xSemaphoreTake(g_core.mutex, portMAX_DELAY);
        g_core.ot_ready = false;
        xSemaphoreGive(g_core.mutex);
        esp_openthread_lock_release();
    }
    if (must_stop) {
        while (!core_wait_sessions_closed(LOST_DRAIN_MS)) {
            ESP_LOGW(TAG, "%s: OpenThread waits for its open sessions before it stops", why);
        }
        return true;
    }
    if (!core_wait_sessions_closed(SESSION_DRAIN_MS)) {
        xSemaphoreTake(g_core.mutex, portMAX_DELAY);
        g_core.ot_ready = true;
        xSemaphoreGive(g_core.mutex);
        return false;
    }
    return true;
}

// A start that failed after OpenThread's init (the border router start failed or timed out, a fault during the init):
// drained as a stop with nothing to go back to, and the border router task, when one was started and has not been
// seen to end (`bringup_ended`), waited for as well, so nothing of the start still uses OpenThread when it is
// deinitialised. That task opens its session before it touches OpenThread and closes it after, so the drain already
// covers it; its end is the belt.
static void drain_failed_start(bool bringup_ended)
{
    close_sessions(true, "the start failed");
    xSemaphoreTake(g_core.mutex, portMAX_DELAY);
    const bool bringup = g_core.bringup_started;
    xSemaphoreGive(g_core.mutex);

    if (bringup && !bringup_ended) {
        radio_step("the border router task's end");
        while (xSemaphoreTake(g_core.bringup_done, pdMS_TO_TICKS(LOST_DRAIN_MS)) != pdTRUE) {
            ESP_LOGW(TAG, "the start failed: OpenThread waits for the border router task before it stops");
        }
    }
}

// Stops OpenThread (the sessions are drained or nothing may wait any more) and forgets its channel.
void core_radio_stop_openthread(void)
{
    esp_br_web_set_openthread_ready(false);
    xSemaphoreTake(g_core.mutex, portMAX_DELAY);
    g_core.ot_ready = false;
    g_core.bringup_started = false;
    xSemaphoreGive(g_core.mutex);
    core_teardown_openthread();
    s_ot_chan = NULL;
    atomic_store(&s_link.ch, NULL);
}

esp_err_t core_radio_ot_start(const char *id)
{
    radio_mux_chan_t *ch = channel_for(id);

    if (!ch) {
        return ESP_ERR_NOT_FOUND;
    }
    if (g_core.init_abandoned) { // an earlier start parked inside esp_openthread_init: nothing of it can be released
        core_set_reason(MODE_FSM_FAULT_ABANDONED);
        return MODE_FSM_ERR_START_ABANDONED;
    }
#ifdef OTBR_TEST_BUILD
    if (core_test_take_start_abandon(id)) { // ot_start_abandon: this start ends as an abandoned one, at once
        ESP_LOGW(TAG, "test build: OpenThread's start on %s abandoned", id);
        g_core.init_abandoned = true; // as a real one: nothing starts OpenThread again until a reboot
        return MODE_FSM_ERR_START_ABANDONED;
    }
#endif
    if (!mode_fsm_radio_start_allowed(g_storage_degraded)) {
        ESP_LOGE(TAG, "storage is unavailable (NVS): OpenThread is not started");
        core_set_reason(MODE_FSM_FAULT_STORAGE);
        return ESP_ERR_INVALID_STATE;
    }
    atomic_store(&s_link.ch, ch);
    atomic_store(&s_link.gone, false);
    s_ot_chan = ch;
    xSemaphoreTake(g_core.mutex, portMAX_DELAY);
    g_core.ot_ready = false;
    g_core.bringup_started = false;
    g_core.fault_reason[0] = '\0';
    xSemaphoreGive(g_core.mutex);
    xSemaphoreTake(g_core.init_done, 0);
    xSemaphoreTake(g_core.bringup_done, 0);

    esp_err_t err = core_start_ot_task();

    radio_step("OpenThread's init");
    // init_done, not boot_done: br_core_start waits for boot_done during the boot, and of two waiters on one binary
    // semaphore only one wakes (the other waited BOOT_WAIT_MS, then abandoned a running OpenThread).
    if (err == ESP_OK && xSemaphoreTake(g_core.init_done, pdMS_TO_TICKS(BOOT_WAIT_MS)) != pdTRUE) {
        g_core.init_abandoned = true; // still inside esp_openthread_init: never torn down under it
        return MODE_FSM_ERR_START_ABANDONED;
    }
    if (err == ESP_OK && !core_ot_ready_now()) {
        err = ESP_FAIL; // the fault mode was entered during the init
    }
    bool bringup_ended = false; // bringup_done taken: the border router task of this start has ended

    if (err == ESP_OK && core_backbone_has_ip6()) { // without an address the border router starts once one comes
        radio_step("the border router start");
        bringup_ended = xSemaphoreTake(g_core.bringup_done, pdMS_TO_TICKS(BRINGUP_WAIT_MS)) == pdTRUE;
        err = bringup_ended && g_core.border_router_up ? ESP_OK : ESP_FAIL;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s: OpenThread did not start: %s", id, esp_err_to_name(err));
        if (!g_core.init_abandoned) {
            drain_failed_start(bringup_ended); // sessions (the border router task's too) end before the deinit
        }
        core_radio_stop_openthread(); // with an abandoned init it releases nothing of it (no deinit)
        if (g_core.init_abandoned) {
            return MODE_FSM_ERR_START_ABANDONED; // an error or a park inside esp_openthread_init
        }
    }
    return err;
}

esp_err_t core_radio_ot_stop(void)
{
    if (!s_ot_chan && !g_core.ot_initialised) {
        atomic_store(&s_link.ch, NULL);
        return ESP_OK; // nothing runs
    }
    // As a push: no new session, Thread REST off, then wait for the open sessions (they end early once it is off). A lost
    // channel has nothing to go back to: the wait takes as long as it takes.
    if (core_ot_ready_now() && !close_sessions(s_lost, "the OTBR channel is gone")) {
        return ESP_ERR_TIMEOUT; // nothing stopped; the caller puts the REST flag back
    }
    ESP_LOGI(TAG, "stopping OpenThread on %s", s_ot_chan ? radio_mux_chan_id(s_ot_chan) : "its channel");
    core_radio_stop_openthread();
    return ESP_OK;
}

esp_err_t core_radio_store_otbr(const char *from, const char *to)
{
    const esp_err_t err = radio_mux_commit_otbr(from, to);

    if (err != ESP_OK) {
        if (!to || !to[0]) { // a stop: `from` leaves otbr for the mode its PUT asked for
            ESP_LOGE(TAG, "storing the stop of otbr on %s: %s", from ? from : "", esp_err_to_name(err));
        } else {
            ESP_LOGE(TAG, "storing otbr on %s (off on %s): %s", to, from && from[0] ? from : "none",
                     esp_err_to_name(err));
        }
        return err;
    }
    s_lease = NULL; // now Thread's binding (the commit's BIND came with the lease's handle)
    input_handled(); // OpenThread already runs on it
    radio_mux_chan_t *ch = atomic_load(&s_attached);

    if (ch) {
        radio_mux_note_firmware(ch, &s_probe_fw);
    }
    return ESP_OK;
}

// After a mode_fsm channel operation: the reason and the Thread REST flag follow the mode it ended in, and
// br_core_start's wait for the radio ends.
static void after_channel_op(void)
{
    const br_core_mode_t mode = br_core_get_mode();

    xSemaphoreTake(g_core.mutex, portMAX_DELAY);
    if (mode == BR_MODE_RADIO_FAULT && g_core.fault_reason[0] == '\0') {
        strlcpy(g_core.fault_reason, s_probe_err[0] ? s_probe_err : FAULT_NO_CHANNEL_START,
                sizeof(g_core.fault_reason));
    } else if (mode == BR_MODE_NO_RADIO) {
        strlcpy(g_core.fault_reason, s_no_radio_reason, sizeof(g_core.fault_reason));
    } else if (mode != BR_MODE_RADIO_FAULT) {
        g_core.fault_reason[0] = '\0';
    }
    const bool ready = g_core.ot_ready;
    xSemaphoreGive(g_core.mutex);
    esp_br_web_set_openthread_ready(ready && mode != BR_MODE_RADIO_FAULT);
    xSemaphoreGive(g_core.boot_done);
}

// Why there is no OpenThread now (the reason NO_RADIO shows), from the channel stored in otbr as the orchestrator sees
// it (mode_fsm_mode_for: no channel in otbr, unplugged, its link fault, ...). A firmware fault of that channel is a
// RADIO_FAULT (design section 6): returned in *fault (may be NULL) with the reason in s_no_radio_reason. Returns whether
// the channel OpenThread ran on (`ran_on`, may be NULL) was lost: it is still the one stored in otbr and no maintenance
// holds it, so it went by an unplug or a failure, not a deliberate change (a mode change, a move, a C6 job), which
// keeps the normal drain.
static bool note_no_radio_reason(radio_mux_chan_t *ran_on, bool *fault)
{
    char otbr[RADIO_MUX_ID_MAX];
    const bool stored = radio_mux_otbr_channel(otbr, sizeof(otbr));
    radio_mux_snapshot_t *snap = stored ? malloc(sizeof(*snap)) : NULL;
    char went[sizeof(s_no_radio_reason)];
    mode_fsm_view_t v = {.bound = stored, .chan_id = otbr, .chan_reason = "", .thread = BR_MODE_NO_RADIO,
                         .thread_reason = went, .unplugged = true, .chan_state = RADIO_MUX_CS_READY
                        };
    bool busy = false, found = false;

    snprintf(went, sizeof(went), "the OTBR channel went away (%s)", stored ? otbr : ""); // ready, but not ours now

    if (fault) {
        *fault = false;
    }
    if (snap) {
        radio_mux_get_snapshot(snap);
        for (size_t i = 0; i < snap->n; i++) {
            const radio_mux_chan_info_t *c = &snap->chans[i];

            if (stored && strcmp(c->id, otbr) == 0) {
                found = true;
                v.chan_state = c->chan_state;
                v.kind = c->fault_kind;
                v.work = c->work;
                busy = c->chan_state == RADIO_MUX_CS_REINIT && c->work != RADIO_MUX_WORK_DETECT; // a C6 job
                if (c->chan_state == RADIO_MUX_CS_FAULT) {
                    strlcpy(s_no_radio_reason, c->chan_reason, sizeof(s_no_radio_reason)); // freed below
                    v.chan_reason = s_no_radio_reason;
                }
            }
        }
    }
    char reason[sizeof(s_no_radio_reason)];
    const br_core_mode_t m = mode_fsm_mode_for(&v, reason, sizeof(reason));

    free(snap);
    if (stored && (!found || m == BR_MODE_STARTING)) { // no snapshot, or a C6 job holds it
        snprintf(reason, sizeof(reason), "%s (%s)", busy ? "a C6 job holds the radio" : "the OTBR channel went away",
                 otbr);
    }
    strlcpy(s_no_radio_reason, reason, sizeof(s_no_radio_reason));
    if (fault) {
        *fault = m == BR_MODE_RADIO_FAULT;
    }
    return ran_on && !busy && stored && strcmp(radio_mux_chan_id(ran_on), otbr) == 0;
}

// The last refused stop of OpenThread on channel `id` and why (NULL: none any more), for the channel's read API.
static void note_refusal(const char *id, const char *reason)
{
    taskENTER_CRITICAL(&s_refusal_lock);
    strlcpy(s_refused_id, id ? id : "", sizeof(s_refused_id));
    strlcpy(s_refused_reason, reason ? reason : "", sizeof(s_refused_reason));
    taskEXIT_CRITICAL(&s_refusal_lock);
}

// The channel's firmware fault (the C6's boot steps found no RCP firmware, the factory image, a failed ROM recovery,
// the test build's c6_hold): RADIO_FAULT with that reason. "rcp incompatible" (an RCP that never answers) clears
// rcp.compatible, as a start that found it so.
static void enter_firmware_fault(const char *reason)
{
    if (strcmp(reason, MODE_FSM_FAULT_INCOMPATIBLE) == 0) {
        core_set_c6_compatible(false);
    }
    s_start_failed = false; // nothing a PUT of mode otbr could retry on: the channel is not usable
    core_enter_fault(reason);
}

// REINIT (maintenance wants Thread's channel): Thread lets go once its sessions drained, or refuses (the sniffer, a
// start, a drain timeout); the answer goes back through the input's handle and is returned (ESP_OK: Thread let go).
static esp_err_t reinit(const radio_mux_input_t *in)
{
    const char *why = NULL;
    esp_err_t verdict;

    radio_busy_begin("OpenThread's release for maintenance");
    note_refusal(NULL, NULL);
    if (g_core.ot_task && !g_core.loop_running) { // after a boot timeout OpenThread may still be initialising
        verdict = ESP_ERR_INVALID_STATE;
        why = "the radio is still booting";
    } else {
        radio_mux_report(atomic_load(&s_report_ch), "releasing", in->reason);
        verdict = mode_fsm_on_reinit(&g_fsm);
        if (verdict == MODE_FSM_ERR_DRAIN_TIMEOUT) {
            verdict = ESP_ERR_INVALID_STATE;
            why = "OpenThread is busy, try again";
        } else if (verdict == ESP_OK) {
            // The maintenance changes what the C6 holds: only the next start on it judges it again.
            xSemaphoreTake(g_core.mutex, portMAX_DELAY);
            g_core.rcp_compatible = false;
            g_core.rcp_compatible_known = false;
            xSemaphoreGive(g_core.mutex);
        }
    }
    ESP_LOGI(TAG, "reinit (%s): %s%s%s", in->reason, verdict == ESP_OK ? "OpenThread let go" : "refused",
             why ? ": " : "", why ? why : "");
    radio_mux_reinit_reply(in->ch, in->seq, verdict, why);
    after_channel_op();
    radio_busy_end();
    return verdict;
}

// OpenThread follows the orchestrator's inputs: a fresh request (REINIT) is answered first; then OpenThread is torn
// down when its channel went (UNBIND, LOST: OpenThread runs on a channel that is not the bound one any more, or its
// link is gone), started on a fresh BIND or BACK (boot, plug-in, the end of a maintenance, a mode change) or a retry (a
// PUT of mode otbr after a failed start). A failed start waits for the next bind. A LOST for a firmware fault of its
// channel (what the C6's boot steps found) is a RADIO_FAULT with the steps' reason. Once Thread let go for a request,
// only a binding input newer than it is acted on (the maintenance's end binds Thread again); a refused request leaves
// the binding input to act on (a BIND it arrived on top of still starts OpenThread).
static void reconcile(void)
{
    radio_mux_input_t req;
    bool let_go = false;

    if (take_input(true, &req) && reinit(&req) == ESP_OK) {
        taskENTER_CRITICAL(&s_in_lock);
        mode_fsm_inbox_let_go(&s_inbox, req.seq);
        taskEXIT_CRITICAL(&s_in_lock);
        let_go = true;
    }
    radio_mux_input_t in;
    const bool fresh = take_input(false, &in);

    if (let_go && !fresh) {
        return; // the maintenance has the channel
    }
    radio_mux_chan_t *att = atomic_load(&s_attached);
    const bool lost_firmware = fresh && in.type == RADIO_MUX_IN_LOST && in.kind == RADIO_MUX_FAULT_FIRMWARE;

    if ((s_ot_chan && (s_ot_chan != att || atomic_load(&s_link.gone))) || (!s_ot_chan && !att && s_start_failed)) {
        bool firmware_fault;

        radio_busy_begin("OpenThread's stop");
        note_refusal(NULL, NULL);
        s_lost = note_no_radio_reason(s_ot_chan, NULL);
        ESP_LOGW(TAG, "%s: NO_RADIO, OpenThread stops", s_no_radio_reason);
        s_start_failed = false;
        mode_fsm_on_channel_gone(&g_fsm); // NO_RADIO first: new sessions are refused while the open ones drain
        s_lost = false;
        // Again after the teardown: the channel's state may have moved on (an unplugged dongle is listed absent only
        // after its LOST).
        note_no_radio_reason(NULL, &firmware_fault);
        if ((lost_firmware || firmware_fault) && br_core_get_mode() == BR_MODE_NO_RADIO) {
            enter_firmware_fault(lost_firmware ? in.reason : s_no_radio_reason);
        }
        after_channel_op();
        radio_busy_end();
    } else if (lost_firmware) {
        enter_firmware_fault(in.reason); // OpenThread did not run on it (the boot, or it had stopped for a maintenance)
        after_channel_op();
    }
    const bool retry = atomic_exchange(&s_retry, false) && s_start_failed; // a PUT of mode otbr (device_api)
    const bool bind = fresh && (in.type == RADIO_MUX_IN_BIND || in.type == RADIO_MUX_IN_BACK);

    if (!s_ot_chan && att && (bind || retry)) {
        radio_busy_begin("the RCP probe");
        note_refusal(NULL, NULL);
        s_probe_err[0] = '\0';
        ESP_LOGI(TAG, "%s: starting OpenThread", radio_mux_chan_id(att));
        mode_fsm_on_channel_back(&g_fsm, radio_mux_chan_id(att));
        s_start_failed = br_core_get_mode() == BR_MODE_RADIO_FAULT;
        after_channel_op();
        radio_busy_end();
    }
}

// The end of a move: its accepter (a PUT that got ESP_OK from br_core_move_otbr) hears of it first, then the next
// move may start.
static void move_finished(void)
{
    void (*done)(void) = atomic_load(&s_move_done);

    if (done && atomic_load(&s_move.state) == MOVE_SIGNALLED && s_move.result == ESP_OK) {
        done();
    }
    atomic_store(&s_move.running, false);
}

static void run_move(void)
{
    char err[RADIO_MUX_REASON_MAX] = "";

    if (atomic_load(&s_move.state) == MOVE_ABANDONED) { // the PUT gave up before br_radio got to it
        atomic_store(&s_move.running, false);
        return;
    }
    const bool stop = s_move.to[0] == '\0';

    note_refusal(NULL, NULL); // a new move, start or stop: an earlier refusal no longer says anything
    ESP_LOGI(TAG, "moving OpenThread from %s to %s", s_move.from[0] ? s_move.from : "(none)",
             stop ? "(none): a stop" : s_move.to);
    s_move_probe = true;
    const esp_err_t ret = mode_fsm_move_otbr(&g_fsm, s_move.from, s_move.to, err, sizeof(err));

    if (s_move_probe) { // refused before the probe
        s_move_probe = false;
        move_signal(ret, err);
    }
    if (s_lease) { // the move failed (a commit ends the lease): OpenThread no longer runs on the target
        const bool nothing_shown = ret == MODE_FSM_ERR_PROBE_FAILED || ret == ESP_ERR_INVALID_STATE;

        radio_mux_lease_end(s_lease, nothing_shown ? NULL : err);
        s_lease = NULL;
    }

    // A failed start on the target whose start back on the source failed too is a fault a PUT of mode otbr retries.
    // Only a move that tried a start says so: one refused before (probe failed, busy, drain timeout) changed nothing,
    // and must not turn an earlier fault (e.g. the C6's "no rcp firmware") into a start failure reconcile acts on.
    if (ret == ESP_OK || ret == MODE_FSM_ERR_START_FAILED) {
        s_start_failed = mode_fsm_start_failed(br_core_get_mode(), s_ot_chan != NULL);
    }
    if (ret == ESP_OK) {
        if (stop) {
            note_no_radio_reason(NULL, NULL); // "no channel in otbr"
        }
        ESP_LOGI(TAG, "OpenThread %s %s", stop ? "stopped on" : "moved to", stop ? s_move.from : s_move.to);
    } else {
        if (stop && ret == MODE_FSM_ERR_DRAIN_TIMEOUT) {
            note_refusal(s_move.from, err); // OpenThread runs on as before: the channel shows why it stayed
        }
        ESP_LOGW(TAG, "%s OpenThread %s %s: %s (%s)", stop ? "stopping" : "moving", stop ? "on" : "to",
                 stop ? s_move.from : s_move.to, esp_err_to_name(ret), err);
    }
    if (ret == MODE_FSM_ERR_START_ABANDONED) {
        // OpenThread's init was abandoned on the target: RADIO_FAULT, as on every other path. No reboot: the binding
        // already names the target, so a reboot would come back there (a PUT of mode otbr on another channel, then a
        // reboot, is the way out).
        ESP_LOGE(TAG, "%s", err);
        core_set_reason(err);
    }
    after_channel_op();
    move_finished();
}

static void radio_task(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (atomic_exchange(&s_move.queued, false)) {
            run_move();
        }
        reconcile();
        report_state();
    }
}

static esp_err_t start_radio_task(void)
{
    TaskHandle_t task;

    if (atomic_load(&s_radio_task)) {
        return ESP_OK;
    }
    if (xTaskCreatePinnedToCore(radio_task, RADIO_TASK_NAME, RADIO_TASK_STACK, NULL, RADIO_TASK_PRIORITY, &task,
                                MGMT_CORE) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    atomic_store(&s_radio_task, task);
    return ESP_OK;
}

// The board's radio at boot (spec: Board mode): no channel stored in otbr, or a dongle, is NO_RADIO until its channel
// is bound; the C6 stays STARTING while its boot steps run (the orchestrator keeps it in reinit), then OpenThread
// starts on its BIND, or its LOST says why it cannot (RADIO_FAULT with the steps' reason).
void core_radio_boot(void)
{
    char id[RADIO_MUX_ID_MAX];
    const bool stored = radio_mux_otbr_channel(id, sizeof(id));
    const bool c6 = mode_fsm_boot(&g_fsm, stored ? id : NULL);

    if (start_radio_task() != ESP_OK) {
        core_enter_fault("cannot create the radio task");
        return;
    }
    if (!c6) {
        snprintf(s_no_radio_reason, sizeof(s_no_radio_reason), "%s%s", stored ? "waiting for " : "no channel in otbr",
                 stored ? id : "");
        core_set_reason(s_no_radio_reason);
        ESP_LOGI(TAG, "NO_RADIO: %s", s_no_radio_reason);
        xSemaphoreGive(g_core.boot_done);
    }
    core_radio_notify(); // a channel bound before the start
}

esp_err_t br_core_register_otbr(void)
{
    static StaticSemaphore_t probed_buf;
    const radio_mux_instance_t inst = {
        .mode = RADIO_MUX_MODE_OTBR,
        .needs = RADIO_MUX_FW_SPINEL,
        .limit = 1,
        .reboot_on_change = false,
        .frames = true,
        .input = on_input,
    };

    if (!s_move.probed) {
        s_move.probed = xSemaphoreCreateBinaryStatic(&probed_buf);
    }
    ESP_RETURN_ON_ERROR(radio_mux_register_instance(&inst), TAG, "the Thread instance");
    radio_mux_set_otbr_mover(br_core_move_otbr);
    return ESP_OK;
}

esp_err_t br_core_move_otbr(const char *from, const char *to, char *reason, size_t cap)
{
    bool idle = false;

    if (reason && cap > 0) {
        reason[0] = '\0';
    }
    if (!g_core.started || !atomic_load(&s_radio_task) || !to || (!to[0] && (!from || !from[0]))) {
        snprintf(reason, cap, "OpenThread is not started");
        return ESP_ERR_INVALID_STATE;
    }
    if (atomic_load(&s_radio_busy)) { // a lost channel's drain or a start runs: its probe would wait for it
        const unsigned busy_s = (unsigned)(xTaskGetTickCount() - atomic_load(&s_radio_busy_since)) /
                                configTICK_RATE_HZ;
        const char *step = atomic_load(&s_radio_step);

        snprintf(reason, cap, "OpenThread is stopping or starting on its radio (%s, %u s)", step, busy_s);
        if (busy_s * 1000 > RADIO_BUSY_WARN_MS) {
            ESP_LOGW(TAG, "br_radio has been busy for %u s, waiting for %s", busy_s, step);
        }
        return ESP_ERR_INVALID_STATE;
    }
    if (!atomic_compare_exchange_strong(&s_move.running, &idle, true)) {
        snprintf(reason, cap, "OpenThread is already being moved");
        return ESP_ERR_INVALID_STATE;
    }
    // A maintenance of the C6 checks s_move.running before it asks: one of them refuses. The C6's boot steps keep a
    // move to the C6 out by its lease (the channel is in reinit).
    if (radio_mux_maintenance_running() && ((from && strcmp(from, RADIO_MUX_BUILTIN_ID) == 0) ||
                                            strcmp(to, RADIO_MUX_BUILTIN_ID) == 0)) {
        atomic_store(&s_move.running, false);
        snprintf(reason, cap, "a C6 firmware push or recovery runs");
        return ESP_ERR_INVALID_STATE;
    }
    strlcpy(s_move.from, from ? from : "", sizeof(s_move.from));
    strlcpy(s_move.to, to, sizeof(s_move.to));
    s_move.result = ESP_FAIL;
    s_move.reason[0] = '\0';
    xSemaphoreTake(s_move.probed, 0);
    atomic_store(&s_move.state, MOVE_WAITING);
    atomic_store(&s_move.queued, true);
    core_radio_notify();
    if (xSemaphoreTake(s_move.probed, pdMS_TO_TICKS(MOVE_PROBE_WAIT_MS)) != pdTRUE) {
        int waiting = MOVE_WAITING;

        if (atomic_compare_exchange_strong(&s_move.state, &waiting, MOVE_ABANDONED)) {
            snprintf(reason, cap, "the probe of %s did not finish in time", to[0] ? to : "(none)");
            return ESP_ERR_TIMEOUT; // br_radio stops the move after its probe
        }
        xSemaphoreTake(s_move.probed, portMAX_DELAY); // signalled just now
    }
    snprintf(reason, cap, "%s", s_move.reason);
    return s_move.result;
}

bool br_core_otbr_refusal(char *id, size_t id_cap, char *reason, size_t reason_cap)
{
    taskENTER_CRITICAL(&s_refusal_lock);
    const bool set = s_refused_id[0] != '\0';

    strlcpy(id, s_refused_id, id_cap);
    strlcpy(reason, s_refused_reason, reason_cap);
    taskEXIT_CRITICAL(&s_refusal_lock);
    return set;
}

bool br_core_move_running(void)
{
    return atomic_load(&s_move.running);
}

void br_core_set_move_done_cb(void (*done)(void))
{
    atomic_store(&s_move_done, done);
}

void br_core_retry_otbr(void)
{
    atomic_store(&s_retry, true);
    core_radio_notify();
}

bool core_radio_input_pending(void)
{
    taskENTER_CRITICAL(&s_in_lock);
    const bool pending = mode_fsm_inbox_pending(&s_inbox);
    taskEXIT_CRITICAL(&s_in_lock);
    return pending;
}

radio_mux_chan_t *core_radio_chan(void)
{
    return atomic_load(&s_link.ch);
}

bool core_radio_gone(void)
{
    return atomic_load(&s_link.gone);
}

void core_radio_mux_set(void)
{
    esp_openthread_mux_set(&s_mux_ops, &s_link);
}
