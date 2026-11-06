/*
 * The console (ESP-IDF console component on UART0, the CH343 bridge of the devkit). Commands:
 *   state          prints the app (project name and version), `light: on|off` and `joined: yes|no`
 *   factory_reset  leaves the network and starts joining again
 */

#include <stdio.h>

#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_console.h"

#include "light.h"
#include "repl.h"
#include "zigbee_node.h"

static const char *TAG = "repl";

static int cmd_state(int argc, char **argv)
{
    const esp_app_desc_t *app = esp_app_get_description();

    printf("app: %s %s\n", app->project_name, app->version);
    printf("light: %s\n", light_is_on() ? "on" : "off");
    printf("joined: %s\n", zigbee_node_is_joined() ? "yes" : "no");
    return 0;
}

static int cmd_factory_reset(int argc, char **argv)
{
    if (zigbee_node_factory_reset() != ESP_OK) {
        printf("factory_reset: failed\n");
        return 1;
    }
    printf("factory_reset: done\n");
    return 0;
}

esp_err_t repl_start(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    esp_console_dev_uart_config_t uart_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    const esp_console_cmd_t state = {
        .command = "state",
        .help = "Print the app, the light and the join state",
        .func = cmd_state,
    };
    const esp_console_cmd_t factory_reset = {
        .command = "factory_reset",
        .help = "Leave the network and start joining again",
        .func = cmd_factory_reset,
    };

    repl_config.prompt = "zbdev>";
    ESP_RETURN_ON_ERROR(esp_console_new_repl_uart(&uart_config, &repl_config, &repl), TAG, "create repl");
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&state), TAG, "register state");
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&factory_reset), TAG, "register factory_reset");
    return esp_console_start_repl(repl);
}
