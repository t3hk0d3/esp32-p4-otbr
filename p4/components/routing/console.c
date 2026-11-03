// Console command `routing show`: the settings and status, in the GET /device/routing shape.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_console.h"
#include "routing.h"

static const char *TAG = "routing_console";

static int cmd_show(void)
{
    routing_settings_t settings;
    routing_status_t status;

    if (routing_get(&settings, &status) != ESP_OK) {
        printf("routing: not started\n");
        return 1;
    }
    char *json = routing_settings_to_json(&settings, &status);
    if (!json) {
        printf("routing: out of memory\n");
        return 1;
    }
    printf("routing: %s\n", json);
    free(json);
    return 0;
}

static int cmd_routing(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "show") == 0) {
        return cmd_show();
    }
    printf("usage: routing show\n");
    return 1;
}

esp_err_t routing_console_register(void)
{
    const esp_console_cmd_t command = {
        .command = "routing",
        .help = "routing show: route method, OMR prefix, BGP sessions and RIPng status",
        .func = cmd_routing,
    };

    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&command), TAG, "registering the routing command failed");
    return ESP_OK;
}
