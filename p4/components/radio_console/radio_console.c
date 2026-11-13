// Console command `radio-fw` (docs/reference/radio-console.md): `radio-fw status` and `radio-fw load <size> <crc32>`.
// The load takes the image into PSRAM over the console UART, then hands it to builtin_rcp_push_firmware. The C6 is not
// touched until the whole image is in and its CRC matches.
#include "radio_console.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board_cores.h"
#include "builtin_rcp.h"
#include "device_api.h"
#include "driver/uart.h"
#include "esp_check.h"
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "radio_console_proto.h"
#include "radio_mux.h"
#include "rcp_sync.h"

static const char *TAG = "radio_console";

#define CONSOLE_UART    CONFIG_ESP_CONSOLE_UART_NUM
#define RX_BUFFER       4096   /* holds several whole frames: one is in flight at a time */
#define READ_CHUNK      256
#define READ_WAIT_MS    50
#define IDLE_LIMIT_MS   10000  /* no frame for this long ends the load */
#define NAK_LIMIT       8      /* naks in a row */
#define BUSY_EVERY_MS   5000
#define PUSH_STACK      6144
#define PUSH_PRIORITY   5

typedef struct {
    const uint8_t *image;
    size_t size;
    esp_err_t err;
    char detail[BUILTIN_RCP_DETAIL_MAX];
    SemaphoreHandle_t done;
} push_job_t;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* One reply line in one write, so another task's log line cannot split it. */
static void reply(const char *kind, const char *fields)
{
    char line[RADIO_CONSOLE_PROTO_LINE_MAX];
    const size_t n = radio_console_proto_line(line, sizeof(line), kind, fields);

    fwrite(line, 1, n, stdout);
    fflush(stdout);
}

static void reply_error(const char *code, const char *detail)
{
    char fields[RADIO_CONSOLE_PROTO_LINE_MAX];

    snprintf(fields, sizeof(fields), "%s %s", code, detail);
    reply("error", fields);
}

static int cmd_status(void)
{
    rcp_sync_probe_result_t probe;
    builtin_rcp_status_t rs;
    char line[RADIO_CONSOLE_PROTO_LINE_MAX];

    rcp_sync_get_last_probe(&probe);
    builtin_rcp_get_status(&rs);
    const radio_console_proto_state_input_t in = {
        .maintenance = radio_mux_maintenance_running(),
        .updater_reachable = probe.updater_reachable,
        .esp_hosted = probe.esp_hosted,
        .failed = probe.last_error[0] != '\0' || probe.rom_recovery_failed,
    };
    const size_t n = radio_console_proto_status_line(line, sizeof(line), radio_console_proto_state(&in),
                                                     rs.installed_known ? rs.installed : "",
                                                     rcp_sync_base_image_version(), rs.jumper);
    fwrite(line, 1, n, stdout);
    fflush(stdout);
    return 0;
}

static esp_err_t memory_read(void *ctx, size_t offset, void *buf, size_t len)
{
    const push_job_t *job = ctx;

    if (offset > job->size || len > job->size - offset) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(buf, job->image + offset, len);
    return ESP_OK;
}

static esp_err_t memory_reopen(void *ctx)
{
    (void)ctx;
    return ESP_OK;
}

static void push_task(void *arg)
{
    push_job_t *job = arg;
    const rcp_fw_source_t source = {
        .total_size = job->size, .read = memory_read, .reopen = memory_reopen, .ctx = job,
    };

    job->err = builtin_rcp_push_firmware(&source, job->detail, sizeof(job->detail));
    xSemaphoreGive(job->done);
    vTaskDelete(NULL);
}

/* Runs the push in its own task and prints `busy <seconds>` until it ends. The job outlives a lost port. */
static int push_image(const uint8_t *image, size_t size)
{
    push_job_t job = {.image = image, .size = size, .done = xSemaphoreCreateBinary()};
    char fields[RADIO_CONSOLE_PROTO_LINE_MAX];
    char line[RADIO_CONSOLE_PROTO_LINE_MAX];

    if (!job.done) {
        reply_error("no_memory", "out of memory");
        return 1;
    }
    if (xTaskCreatePinnedToCore(push_task, "radio_console", PUSH_STACK, &job, PUSH_PRIORITY, NULL, MGMT_CORE) != pdPASS) {
        vSemaphoreDelete(job.done);
        reply_error("no_memory", "cannot start the push task");
        return 1;
    }
    const uint32_t start = now_ms();
    while (xSemaphoreTake(job.done, pdMS_TO_TICKS(BUSY_EVERY_MS)) != pdTRUE) {
        snprintf(fields, sizeof(fields), "%u", (unsigned)((now_ms() - start) / 1000));
        reply("busy", fields);
    }
    vSemaphoreDelete(job.done);

    builtin_rcp_status_t rs;
    builtin_rcp_get_status(&rs);
    const char *code = job.err == ESP_OK ? NULL : device_api_push_error_code(job.err);
    const size_t n = radio_console_proto_result_line(line, sizeof(line), job.err, code, job.detail,
                                                     rs.installed_known ? rs.installed : "");
    fwrite(line, 1, n, stdout);
    fflush(stdout);
    ESP_LOGI(TAG, "push ended: %s", esp_err_to_name(job.err));
    return job.err == ESP_OK ? 0 : 1;
}

static const char *nak_reason(radio_console_proto_event_t e)
{
    switch (e) {
    case RADIO_CONSOLE_PROTO_NAK_CRC:
        return "crc";
    case RADIO_CONSOLE_PROTO_NAK_LENGTH:
        return "length";
    case RADIO_CONSOLE_PROTO_NAK_TIMEOUT:
        return "timeout";
    default:
        return "offset";
    }
}

typedef enum {
    RECEIVE_DONE,
    RECEIVE_TIMEOUT,
    RECEIVE_ABORTED,
    RECEIVE_FAILED,
} receive_end_t;

typedef struct {
    uint8_t *image;
    size_t size;
    size_t expected; // bytes taken so far
    int naks;        // in a row
    uint32_t last_frame;
    radio_console_proto_parser_t parser;
} receiver_t;

/* Replies to one parser event; false when it ends the load (abort, or too many naks). */
static bool handle(receiver_t *r, radio_console_proto_event_t e, uint32_t now, receive_end_t *end)
{
    char fields[32];

    if (e == RADIO_CONSOLE_PROTO_NONE) {
        return true;
    }
    r->last_frame = now;
    if (e == RADIO_CONSOLE_PROTO_ABORT) {
        *end = RECEIVE_ABORTED;
        return false;
    }
    if (e == RADIO_CONSOLE_PROTO_FRAME) {
        size_t next;
        const radio_console_proto_accept_t a =
            radio_console_proto_accept(r->expected, r->size, r->parser.offset, r->parser.len, &next);
        if (a != RADIO_CONSOLE_PROTO_NAK_OFFSET) {
            if (a == RADIO_CONSOLE_PROTO_WRITE) {
                memcpy(r->image + r->expected, r->parser.payload, r->parser.len);
            }
            r->expected = next;
            r->naks = 0;
            snprintf(fields, sizeof(fields), "%u", (unsigned)r->expected);
            reply("ack", fields);
            return true;
        }
    }
    snprintf(fields, sizeof(fields), "%u %s", (unsigned)r->expected, nak_reason(e));
    reply("nak", fields);
    if (++r->naks > NAK_LIMIT) {
        *end = RECEIVE_FAILED;
        return false;
    }
    return true;
}

/* Takes frames into r->image until it holds r->size bytes, replying ack or nak to each. */
static receive_end_t receive(receiver_t *r)
{
    uint8_t chunk[READ_CHUNK];
    receive_end_t end = RECEIVE_DONE;

    radio_console_proto_parser_init(&r->parser);
    r->last_frame = now_ms();
    while (r->expected < r->size) {
        const int got = uart_read_bytes(CONSOLE_UART, chunk, sizeof(chunk), pdMS_TO_TICKS(READ_WAIT_MS));
        const uint32_t now = now_ms();

        if (!handle(r, radio_console_proto_poll(&r->parser, now), now, &end)) {
            return end;
        }
        for (int i = 0; i < got && r->expected < r->size; i++) {
            if (!handle(r, radio_console_proto_feed(&r->parser, chunk[i], now), now, &end)) {
                return end;
            }
        }
        if (now - r->last_frame > IDLE_LIMIT_MS) {
            return RECEIVE_TIMEOUT;
        }
    }
    return RECEIVE_DONE;
}

static int cmd_load(int argc, char **argv)
{
    size_t size;
    uint32_t crc;
    const esp_err_t parse = radio_console_proto_parse_load(argc, argv, &size, &crc);

    if (parse == ESP_ERR_INVALID_SIZE) {
        reply_error("image_too_large", "size is 0 or exceeds the C6 slot");
        return 1;
    }
    if (parse != ESP_OK) {
        reply_error("invalid_request", "usage: radio-fw load <size> <crc32 hex>");
        return 1;
    }
    if (radio_mux_maintenance_running()) {
        reply_error("busy", "a radio maintenance is running");
        return 1;
    }
    uint8_t *image = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    receiver_t *r = calloc(1, sizeof(*r));
    if (!image || !r) {
        free(image);
        free(r);
        reply_error("no_memory", "no buffer for the image");
        return 1;
    }
    // The REPL reads UART0 without a driver; the driver's ring buffer holds a whole frame while this task is busy.
    // The REPL task runs this handler, so nothing else reads the UART meanwhile.
    esp_err_t err = uart_driver_install(CONSOLE_UART, RX_BUFFER, 0, 0, NULL, 0);
    if (err != ESP_OK) {
        free(image);
        free(r);
        reply_error("push_failed", "cannot take the console UART");
        return 1;
    }
    uart_flush_input(CONSOLE_UART);
    reply("ready", "max=1024");
    r->image = image;
    r->size = size;
    const receive_end_t end = receive(r);
    uart_driver_delete(CONSOLE_UART);
    free(r);

    int result = 1;
    if (end == RECEIVE_TIMEOUT) {
        reply_error("timeout", "no frame for 10 s");
    } else if (end == RECEIVE_ABORTED) {
        reply_error("aborted", "the sender stopped");
    } else if (end == RECEIVE_FAILED) {
        reply_error("transfer", "too many naks in a row");
    } else if (radio_console_proto_crc32(0, image, size) != crc) {
        reply_error("invalid_request", "image checksum mismatch");
    } else {
        reply("received", NULL);
        ESP_LOGI(TAG, "image of %u bytes received, pushing it to the radio", (unsigned)size);
        result = push_image(image, size);
    }
    free(image);
    return result;
}

static int cmd_radio_fw(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "status") == 0) {
        return cmd_status();
    }
    if (argc >= 2 && strcmp(argv[1], "load") == 0) {
        return cmd_load(argc, argv);
    }
    printf("usage: radio-fw status | radio-fw load <size> <crc32 hex> (see docs/reference/radio-console.md)\n");
    return 1;
}

esp_err_t radio_console_register(void)
{
    const esp_console_cmd_t command = {
        .command = "radio-fw",
        .help = "radio-fw status: the C6's state; radio-fw load <size> <crc32>: take a C6 image over this port (the "
        "browser flasher's protocol) and install it",
        .func = cmd_radio_fw,
    };

    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&command), TAG, "registering the radio-fw command failed");
    return ESP_OK;
}
