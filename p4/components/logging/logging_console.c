// Console command `log`: show and set the logging settings from the UART when the network is down.
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_console.h"
#include "logging.h"
#include "logging_core.h"

static const char *TAG = "logging";

#define USAGE                                                                                                     \
    "usage: log\n"                                                                                                \
    "       log level <uart|jtag|syslog> <off|error|warning|info|debug|verbose>\n"                                \
    "       log tag <tag|*> <level|default>   (* = everything else; default removes the override)\n"              \
    "       log syslog <host[:port]|off>\n"

// The REPL runs one command at a time on its own task, whose stack is small: the settings (about 1 KB) stay static.
static logging_settings_t s_settings;

static const char *syslog_state_name(logging_syslog_state_t state)
{
    switch (state) {
    case LOGGING_SYSLOG_RESOLVING:
        return "resolving";
    case LOGGING_SYSLOG_SENDING:
        return "sending";
    case LOGGING_SYSLOG_UNRESOLVED:
        return "unresolved";
    case LOGGING_SYSLOG_NO_SOCKET:
        return "no_socket";
    default:
        return "off";
    }
}

static int cmd_show(void)
{
    static const char *const names[LOGGING_CH_COUNT] = {"uart", "jtag", "syslog"};
    logging_status_t status;

    if (logging_get_settings(&s_settings) != ESP_OK) {
        printf("log: not started\n");
        return 1;
    }
    logging_get_status(&status);
    printf("log: channel  level    sent  dropped\n");
    for (int ch = 0; ch < LOGGING_CH_COUNT; ch++) {
        printf("log:   %-7s %-8s %u  %u\n", names[ch], logging_level_name(s_settings.channel[ch]),
               (unsigned) status.sent[ch], (unsigned) status.dropped[ch]);
    }
    printf("log: syslog target: %s (%s)\n", s_settings.syslog_target[0] ? s_settings.syslog_target : "none",
           syslog_state_name(status.syslog));
    printf("log: usb-jtag host: %s\n", status.jtag_host ? "reading" : "none");
    printf("log: live viewers: %u, lines sent %u, dropped %u\n", (unsigned) status.viewers,
           (unsigned) status.live_sent, (unsigned) status.live_dropped);
    printf("log: highest level in this build: %s\n", logging_level_name(logging_level_ceiling()));
    printf("log: default (everything else): %s\n", logging_level_name(s_settings.default_level));
    for (int i = 0; i < s_settings.n_components; i++) {
        printf("log: override %s: %s\n", s_settings.components[i].tag,
               logging_level_name(s_settings.components[i].level));
    }
    return 0;
}

static int cmd_set(int argc, char **argv)
{
    const char *err = NULL;
    const char *field = NULL;

    if (logging_get_settings(&s_settings) != ESP_OK) {
        printf("log: not started\n");
        return 1;
    }
    if (logging_console_parse(argc, argv, &s_settings, &err) != ESP_OK) {
        printf("log: %s\n" USAGE, err ? err : "bad arguments");
        return 1;
    }
    switch (logging_apply(&s_settings, &field)) {
    case ESP_OK:
        break;
    case ESP_ERR_INVALID_ARG:
        printf("log: refused, %s is invalid (at most %s in this build, %s for *; syslog needs a target before its level "
               "can be raised)\n",
               field ? field : "a field", logging_level_name(logging_level_ceiling()),
               logging_level_name(logging_core_default_max(logging_level_ceiling())));
        return 1;
    default:
        printf("log: in force, but not stored (lost at reboot)\n");
        return 1;
    }
    printf("log: done\n");
    if (strcmp(argv[1], "syslog") == 0 && strcmp(argv[2], "off") != 0 &&
            s_settings.channel[LOGGING_CH_SYSLOG] == ESP_LOG_NONE) {
        printf("log: syslog level is still off; turn it on with: log level syslog <level>\n");
    }
    return 0;
}

static int cmd_log(int argc, char **argv)
{
    if (argc == 1) {
        return cmd_show();
    }
    return cmd_set(argc, argv);
}

esp_err_t logging_console_register(void)
{
    const esp_console_cmd_t command = {
        .command = "log",
        .help = "log: show levels, syslog target, overrides and counters; "
        "log level <uart|jtag|syslog> <level>; log tag <tag|*> <level|default>; log syslog <host[:port]|off>",
        .func = cmd_log,
    };

    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&command), TAG, "registering the log command failed");
    return ESP_OK;
}
