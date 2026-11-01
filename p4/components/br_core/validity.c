#include "br_core.h"

#include "esp_br_web.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"

static const char *TAG = "validity";

#define VALID_AFTER_US (60 * 1000000LL)
#define RECHECK_US (1000 * 1000LL)
#define RETRY_US (5 * 1000 * 1000LL)

static esp_timer_handle_t s_timer;

static bool pending_verify(void)
{
    esp_ota_img_states_t state;

    return esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) == ESP_OK &&
           state == ESP_OTA_IMG_PENDING_VERIFY;
}

// Criteria (spec 5.3 step 7): 60 s without reset and the HTTP server listening. Nothing else.
static void on_timer(void *arg)
{
    if (!pending_verify()) {
        ESP_LOGI(TAG, "image is not pending verification");
        return;
    }
    if (!esp_br_web_is_server_started()) {
        esp_timer_start_once(s_timer, RECHECK_US);
        return;
    }
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "app marked valid");
    } else {
        ESP_LOGE(TAG, "marking the app valid failed: %s", esp_err_to_name(err));
        esp_timer_start_once(s_timer, RETRY_US);
    }
}

void validity_start(void)
{
    const esp_timer_create_args_t args = {.callback = on_timer, .name = "validity"};
    int64_t remaining = VALID_AFTER_US - esp_timer_get_time();

    if (remaining < 0) {
        remaining = 0;
    }
    if (esp_timer_create(&args, &s_timer) != ESP_OK || esp_timer_start_once(s_timer, remaining) != ESP_OK) {
        ESP_LOGE(TAG, "cannot start the validity timer");
    }
}
