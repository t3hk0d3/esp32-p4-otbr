#include "console.h"

#include "board_cores.h"
#include "esp_check.h"
#include "esp_console.h"
#include "logging.h"
#include "netcfg.h"
#include "radio_console.h"
#include "routing.h"
#ifdef OTBR_TEST_BUILD
#include "test_cmds.h"
#endif

static const char *TAG = "console";

esp_err_t console_start(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "p4> ";
    repl_config.task_core_id = MGMT_CORE; // task "console_repl": management, not the network core
    esp_console_dev_uart_config_t uart_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();

    ESP_RETURN_ON_ERROR(esp_console_new_repl_uart(&uart_config, &repl_config, &repl), TAG, "creating REPL failed");
    ESP_RETURN_ON_ERROR(netcfg_console_register(), TAG, "net command");
    ESP_RETURN_ON_ERROR(routing_console_register(), TAG, "routing command");
    ESP_RETURN_ON_ERROR(logging_console_register(), TAG, "log command");
    ESP_RETURN_ON_ERROR(radio_console_register(), TAG, "radio-fw command");
#ifdef OTBR_TEST_BUILD
    ESP_RETURN_ON_ERROR(test_cmds_register(), TAG, "test commands");
#endif
    return esp_console_start_repl(repl);
}
