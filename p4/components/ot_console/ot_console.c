// Thread console board glue: the setting, OpenThread's CLI output callback, the input path and the USB `ot` command.
// The decisions (which input may reach the CLI, where output goes) are in core.c.
#include "ot_console.h"

#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "br_core.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_openthread.h"
#include "esp_openthread_cli.h"
#include "esp_openthread_lock.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "netnames.h"
#include "nvs.h"
#include "openthread/cli.h"
#include "openthread/instance.h"
#include "openthread/thread.h"
#include "ot_console_core.h"
#ifdef OTBR_TEST_BUILD
#include "esp_console.h"
#endif

static const char *TAG = "ot_console";

#define NVS_NAMESPACE "console"
#define KEY_ENABLED "enabled"
// OPENTHREAD_CONFIG_CLI_MAX_LINE_LENGTH is in OpenThread's private config (384, or 640 in reference-device builds); the
// smaller one fits both.
#define CLI_LINE_MAX (384 - 1)
#define OUTPUT_CAP (8 * 1024)
#define NOTICE_LEN 448
#define NOTICE_SLOTS 4
#define PEER_LEN 48
#define USB_TIMEOUT_MS 10000
#define LOCK_WAIT_MS 1000

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_enabled;
static ot_console_arb_t s_arb;
static uint8_t s_storage[OUTPUT_CAP];
static ot_console_buf_t s_out;
static struct {
    bool attached;
    uint32_t id;
    char peer[PEER_LEN];
    int64_t since_us;
    ot_console_sink_t sink;
    char notices[NOTICE_SLOTS][NOTICE_LEN]; // waiting notices, oldest first
    size_t notice_count;
} s_session;
static uint32_t s_next_id = 1;
static SemaphoreHandle_t s_usb_done; // given at the prompt of a USB line
static atomic_bool s_usb_waiting;

static const char *mode_name(br_core_mode_t mode)
{
    switch (mode) {
    case BR_MODE_STARTING:
        return "STARTING";
    case BR_MODE_BR_RUNNING:
        return "BR_RUNNING";
    case BR_MODE_SNIFFER:
        return "SNIFFER";
    case BR_MODE_RADIO_FAULT:
        return "RADIO_FAULT";
    }
    return "UNKNOWN";
}

// With s_mux held. A full queue drops the newest notice: the oldest say why the session looks the way it does.
static void queue_notice_locked(const char *text)
{
    if (s_session.notice_count < NOTICE_SLOTS) {
        strlcpy(s_session.notices[s_session.notice_count++], text, NOTICE_LEN);
    }
}

// Queue a notice for the session `id` and wake its transport.
static void notice(uint32_t id, const char *text)
{
    ot_console_sink_t sink = {0};

    taskENTER_CRITICAL(&s_mux);
    if (s_session.attached && s_session.id == id) {
        queue_notice_locked(text);
        sink = s_session.sink;
    }
    taskEXIT_CRITICAL(&s_mux);
    if (sink.wake) {
        sink.wake(sink.ctx);
    }
}

// The prompt or a stack stop ended the pending line: a USB command waiting for it returns.
static void usb_done(void)
{
    if (atomic_load(&s_usb_waiting) && s_usb_done) {
        xSemaphoreGive(s_usb_done);
    }
}

static void route(const char *text, size_t len)
{
    ot_console_span_t span;
    ot_console_sink_t sink = {0};

    taskENTER_CRITICAL(&s_mux);
    const ot_console_dest_t dest = ot_console_arb_output(&s_arb, text, len, &span);

    if (dest == OT_CONSOLE_TO_SESSION) {
        if (span.flush_carry) {
            ot_console_buf_push(&s_out, ">", 1);
        }
        ot_console_buf_push(&s_out, text, span.len);
        sink = s_session.sink;
    }
    taskEXIT_CRITICAL(&s_mux);

    switch (dest) {
    case OT_CONSOLE_TO_SESSION:
        if (sink.wake) {
            sink.wake(sink.ctx);
        }
        break;
    case OT_CONSOLE_TO_STDOUT:
        if (span.flush_carry) {
            fputc('>', stdout);
        }
        fwrite(text, 1, span.len, stdout);
        break;
    case OT_CONSOLE_DROP_PROMPT:
        usb_done();
        break;
    }
}

// OpenThread's CLI output, on the OpenThread task: formats and routes, never blocks.
static int output_cb(void *context, const char *format, va_list args)
{
    (void)context;
    char text[CLI_LINE_MAX + 1];
    va_list copy;

    va_copy(copy, args);
    const int n = vsnprintf(text, sizeof(text), format, copy);
    va_end(copy);
    if (n < 0) {
        return n;
    }
    if ((size_t)n < sizeof(text)) {
        route(text, (size_t)n);
        return n;
    }
    char *big = malloc((size_t)n + 1);

    if (!big) {
        route(text, sizeof(text) - 1); // the start of it rather than nothing
        return n;
    }
    vsnprintf(big, (size_t)n + 1, format, args);
    route(big, (size_t)n);
    free(big);
    return n;
}

// br_core's CLI hook: true on the bring-up task with the OpenThread lock held, false before OpenThread stops.
static void cli_hook(bool running)
{
    if (running) {
        otCliInit(esp_openthread_get_instance(), output_cb, NULL);
    }
    taskENTER_CRITICAL(&s_mux);
    ot_console_arb_stack(&s_arb, running);
    taskEXIT_CRITICAL(&s_mux);
    usb_done();
}

// Post a line the arbiter let through. False when it could not be posted (the pending line is cleared again).
static bool post(const char *line)
{
    bool posted = false;

    if (br_core_ot_session_begin()) {
        posted = esp_openthread_cli_input(line) == ESP_OK;
        br_core_ot_session_end();
    }
    if (!posted) {
        taskENTER_CRITICAL(&s_mux);
        ot_console_arb_stack(&s_arb, s_arb.running); // clears the pending line it recorded
        taskEXIT_CRITICAL(&s_mux);
    }
    return posted;
}

esp_err_t ot_console_init(void)
{
    nvs_handle_t nvs;
    const esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);

    if (err == ESP_OK) {
        uint8_t value;

        if (nvs_get_u8(nvs, KEY_ENABLED, &value) == ESP_OK) {
            s_enabled = value != 0;
        }
        nvs_close(nvs);
    } else if (err != ESP_ERR_NVS_NOT_FOUND) { // not found: nothing stored yet
        ESP_LOGE(TAG, "reading the setting failed (%s): off", esp_err_to_name(err));
    }
    ot_console_arb_init(&s_arb);
    ot_console_buf_init(&s_out, s_storage, sizeof(s_storage));
    s_usb_done = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_usb_done, ESP_ERR_NO_MEM, TAG, "semaphore");
    br_core_set_ot_cli_hook(cli_hook);
    return ESP_OK;
}

bool ot_console_enabled(void)
{
    taskENTER_CRITICAL(&s_mux);
    const bool on = s_enabled;
    taskEXIT_CRITICAL(&s_mux);
    return on;
}

esp_err_t ot_console_set_enabled(bool on)
{
    nvs_handle_t nvs;

    ESP_RETURN_ON_ERROR(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs), TAG, "nvs_open");
    esp_err_t err = nvs_set_u8(nvs, KEY_ENABLED, on ? 1 : 0);

    err = err == ESP_OK ? nvs_commit(nvs) : err;
    nvs_close(nvs);
    ESP_RETURN_ON_ERROR(err, TAG, "storing the setting");

    ot_console_sink_t closing = {0};

    taskENTER_CRITICAL(&s_mux);
    s_enabled = on;
    if (!on && s_session.attached) {
        closing = s_session.sink;
        s_session.attached = false;
        ot_console_arb_attach(&s_arb, false);
    }
    taskEXIT_CRITICAL(&s_mux);
    ESP_LOGI(TAG, "console %s", on ? "on" : "off");
    if (closing.close) {
        char text[NOTICE_LEN];

        ot_console_notice_turned_off(text, sizeof(text));
        closing.close(closing.ctx, OT_CONSOLE_CLOSE_TURNED_OFF, text);
    }
    return ESP_OK;
}

// The first notice of a session: who and what it is talking to, or why Thread does not answer.
static void greeting(char *out, size_t cap)
{
    const br_core_mode_t mode = br_core_get_mode();

    if (mode == BR_MODE_BR_RUNNING && br_core_ot_session_begin()) {
        if (esp_openthread_lock_acquire(pdMS_TO_TICKS(LOCK_WAIT_MS))) {
            otInstance *instance = esp_openthread_get_instance();

            ot_console_notice_connected(out, cap, netnames_hostname(), otGetVersionString(),
                                        otThreadDeviceRoleToString(otThreadGetDeviceRole(instance)),
                                        otThreadGetRloc16(instance));
            esp_openthread_lock_release();
            br_core_ot_session_end();
            return;
        }
        br_core_ot_session_end();
    }
    ot_console_notice_not_running(out, cap, mode_name(mode));
}

esp_err_t ot_console_attach(const char *peer, ot_console_sink_t sink, uint32_t *session_id)
{
    ot_console_sink_t old = {0};
    char text[NOTICE_LEN];

    taskENTER_CRITICAL(&s_mux);
    if (!s_enabled) {
        taskEXIT_CRITICAL(&s_mux);
        return ESP_ERR_INVALID_STATE;
    }
    if (s_session.attached) {
        old = s_session.sink;
    }
    s_session.attached = true;
    s_session.id = s_next_id++;
    strlcpy(s_session.peer, peer, sizeof(s_session.peer));
    s_session.since_us = esp_timer_get_time();
    s_session.sink = sink;
    s_session.notice_count = 0;
    ot_console_buf_init(&s_out, s_storage, sizeof(s_storage));
    ot_console_arb_attach(&s_arb, true);
    *session_id = s_session.id;
    const uint32_t id = s_session.id;
    taskEXIT_CRITICAL(&s_mux);

    ESP_LOGI(TAG, "console session from %s", peer);
    if (old.close) {
        ot_console_notice_taken_over(text, sizeof(text), peer);
        old.close(old.ctx, OT_CONSOLE_CLOSE_TAKEN_OVER, text);
    }
    greeting(text, sizeof(text));
    notice(id, text);
    return ESP_OK;
}

void ot_console_detach(uint32_t session_id)
{
    bool ended = false;

    taskENTER_CRITICAL(&s_mux);
    if (s_session.attached && s_session.id == session_id) {
        s_session.attached = false;
        ot_console_arb_attach(&s_arb, false);
        ended = true;
    }
    taskEXIT_CRITICAL(&s_mux);
    if (ended) {
        ESP_LOGI(TAG, "console session ended");
    }
}

esp_err_t ot_console_submit(uint32_t session_id, const char *line)
{
    char text[NOTICE_LEN];
    char copy[OT_CONSOLE_PENDING_LEN];
    char peer[PEER_LEN];

    taskENTER_CRITICAL(&s_mux);
    const bool current = s_session.attached && s_session.id == session_id;

    if (current) {
        strlcpy(peer, s_session.peer, sizeof(peer));
    }
    taskEXIT_CRITICAL(&s_mux);
    ESP_RETURN_ON_FALSE(current, ESP_ERR_INVALID_STATE, TAG, "not the current session");

    if (strlcpy(copy, line, sizeof(copy)) >= sizeof(copy)) {
        ot_console_notice_too_long(text, sizeof(text), CLI_LINE_MAX);
        notice(session_id, text);
        return ESP_OK;
    }
    switch (ot_console_line_normalise(copy, CLI_LINE_MAX)) {
    case OT_CONSOLE_LINE_EMPTY:
        return ESP_OK;
    case OT_CONSOLE_LINE_TOO_LONG:
        ot_console_notice_too_long(text, sizeof(text), CLI_LINE_MAX);
        notice(session_id, text);
        return ESP_OK;
    case OT_CONSOLE_LINE_OK:
        break;
    }
    const br_core_mode_t mode = br_core_get_mode();

    if (mode != BR_MODE_BR_RUNNING) {
        ot_console_notice_not_running(text, sizeof(text), mode_name(mode));
        notice(session_id, text);
        return ESP_OK;
    }
    char pending[OT_CONSOLE_PENDING_LEN] = "";

    taskENTER_CRITICAL(&s_mux);
    const ot_console_verdict_t verdict = ot_console_arb_submit(&s_arb, OT_CONSOLE_SRC_WS, copy);

    if (verdict == OT_CONSOLE_BUSY) {
        strlcpy(pending, ot_console_arb_pending(&s_arb), sizeof(pending));
    }
    taskEXIT_CRITICAL(&s_mux);

    switch (verdict) {
    case OT_CONSOLE_SEND:
        ESP_LOGI(TAG, "console %s: %s", peer, copy);
        if (!post(copy)) {
            ot_console_notice_not_running(text, sizeof(text), mode_name(br_core_get_mode()));
            notice(session_id, text);
        }
        break;
    case OT_CONSOLE_BUSY:
        ot_console_notice_busy(text, sizeof(text), pending);
        notice(session_id, text);
        break;
    case OT_CONSOLE_NOT_RUNNING:
    case OT_CONSOLE_IN_USE: // only for USB lines
        ot_console_notice_not_running(text, sizeof(text), mode_name(mode));
        notice(session_id, text);
        break;
    }
    return ESP_OK;
}

size_t ot_console_take(uint32_t session_id, char *out, size_t cap)
{
    size_t n = 0;

    taskENTER_CRITICAL(&s_mux);
    if (!s_session.attached || s_session.id != session_id || cap == 0) {
        taskEXIT_CRITICAL(&s_mux);
        return 0;
    }
    const size_t dropped = ot_console_buf_take_dropped(&s_out);

    if (dropped > 0) {
        const int len = ot_console_notice_dropped(out, cap, dropped);

        n = len < 0 ? 0 : ((size_t)len < cap ? (size_t)len : cap - 1);
    } else if (s_out.len > 0) {
        n = ot_console_buf_take(&s_out, out, cap);
    } else if (s_session.notice_count > 0) {
        n = strlcpy(out, s_session.notices[0], cap);
        n = n < cap ? n : cap - 1;
        s_session.notice_count--;
        memmove(s_session.notices[0], s_session.notices[1], s_session.notice_count * NOTICE_LEN);
    }
    taskEXIT_CRITICAL(&s_mux);
    return n;
}

size_t ot_console_queued(uint32_t session_id)
{
    size_t n = 0;

    taskENTER_CRITICAL(&s_mux);
    if (s_session.attached && s_session.id == session_id) {
        n = s_out.len + s_session.notice_count;
    }
    taskEXIT_CRITICAL(&s_mux);
    return n;
}

bool ot_console_session(char *peer, size_t peer_cap, uint32_t *since_s)
{
    taskENTER_CRITICAL(&s_mux);
    const bool attached = s_session.attached;

    if (attached && peer) {
        strlcpy(peer, s_session.peer, peer_cap);
    }
    if (attached && since_s) {
        *since_s = (uint32_t)((esp_timer_get_time() - s_session.since_us) / 1000000);
    }
    taskEXIT_CRITICAL(&s_mux);
    return attached;
}

#ifdef OTBR_TEST_BUILD
// Console `ot <command>`: the same arbiter as the network session; waits for the prompt, at most 10 s.
static int usb_ot_command(int argc, char **argv)
{
    char line[OT_CONSOLE_PENDING_LEN] = "";
    char peer[PEER_LEN] = "";
    char pending[OT_CONSOLE_PENDING_LEN] = "";

    for (int i = 1; i < argc; i++) {
        if (i > 1) {
            strlcat(line, " ", sizeof(line));
        }
        if (strlcat(line, argv[i], sizeof(line)) >= sizeof(line)) {
            printf("Line too long (max %d bytes)\n", CLI_LINE_MAX);
            return 0;
        }
    }
    switch (ot_console_line_normalise(line, CLI_LINE_MAX)) {
    case OT_CONSOLE_LINE_EMPTY:
        printf("Usage: ot <OpenThread CLI command>\n");
        return 0;
    case OT_CONSOLE_LINE_TOO_LONG:
        printf("Line too long (max %d bytes)\n", CLI_LINE_MAX);
        return 0;
    case OT_CONSOLE_LINE_OK:
        break;
    }
    taskENTER_CRITICAL(&s_mux);
    const ot_console_verdict_t verdict = ot_console_arb_submit(&s_arb, OT_CONSOLE_SRC_USB, line);

    if (verdict == OT_CONSOLE_IN_USE) {
        strlcpy(peer, s_session.peer, sizeof(peer));
    } else if (verdict == OT_CONSOLE_BUSY) {
        strlcpy(pending, ot_console_arb_pending(&s_arb), sizeof(pending));
    }
    taskEXIT_CRITICAL(&s_mux);

    switch (verdict) {
    case OT_CONSOLE_IN_USE:
        printf("console in use from %s\n", peer);
        return 0;
    case OT_CONSOLE_NOT_RUNNING:
        printf("Error 13: InvalidState (OpenThread not running)\n");
        return 0;
    case OT_CONSOLE_BUSY:
        printf("Busy: \"%s\" is still running\n", pending);
        return 0;
    case OT_CONSOLE_SEND:
        break;
    }
    ESP_LOGI(TAG, "console usb: %s", line);
    xSemaphoreTake(s_usb_done, 0); // a give left from an earlier command that timed out
    atomic_store(&s_usb_waiting, true);
    if (!post(line)) {
        atomic_store(&s_usb_waiting, false);
        printf("Error 13: InvalidState (OpenThread not running)\n");
        return 0;
    }
    if (xSemaphoreTake(s_usb_done, pdMS_TO_TICKS(USB_TIMEOUT_MS)) != pdTRUE) {
        printf("No prompt after %d s: \"%s\" is still running, its output follows when it comes\n",
               USB_TIMEOUT_MS / 1000, line);
    }
    atomic_store(&s_usb_waiting, false);
    return 0;
}

esp_err_t ot_console_usb_command_register(void)
{
    const esp_console_cmd_t command = {
        .command = "ot",
        .help = "OpenThread CLI command (the same CLI as the Thread console; waits at most 10 s for it)",
        .func = usb_ot_command,
    };

    return esp_console_cmd_register(&command);
}
#endif // OTBR_TEST_BUILD
