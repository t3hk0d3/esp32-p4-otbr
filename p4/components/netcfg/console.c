// Console command `net` in every build (spec 5.13 recovery): `net show`, `net reset`.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_console.h"
#include "netcfg.h"

static const char *TAG = "netcfg_console";

static int cmd_show(void)
{
    char *json = netcfg_state_json();

    if (!json) {
        printf("net: not available (not started, or out of memory)\n");
        return 1;
    }
    printf("net: %s\n", json);
    free(json);
    return 0;
}

static int cmd_reset(void)
{
    const esp_err_t err = netcfg_reset();

    if (err != ESP_OK) {
        printf("net: reset failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("net: defaults restored (single, untagged, DHCPv4), rebooting\n");
    return 0;
}

static int cmd_net(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "show") == 0) {
        return cmd_show();
    }
    if (argc == 2 && strcmp(argv[1], "reset") == 0) {
        return cmd_reset();
    }
    printf("usage: net show | net reset\n");
    return 1;
}

esp_err_t netcfg_console_register(void)
{
    const esp_console_cmd_t command = {
        .command = "net",
        .help = "net show: active config, trial and interfaces; net reset: restore single/untagged/DHCPv4 and reboot",
        .func = cmd_net,
    };

    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&command), TAG, "registering the net command failed");
    return ESP_OK;
}
