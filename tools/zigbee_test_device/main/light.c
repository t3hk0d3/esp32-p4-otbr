/*
 * The On/Off cluster's light: the devkit's on-board WS2812B RGB LED (net RGB_CTRL, GPIO8), driven by espressif/led_strip
 * over RMT. The `light: on` / `light: off` console lines are what the pytest helper reads.
 */

#include <stdio.h>

#include "esp_check.h"
#include "led_strip.h"

#include "light.h"

#define LIGHT_LED_GPIO          8
#define LIGHT_LED_COUNT         1
#define LIGHT_RMT_RESOLUTION_HZ (10 * 1000 * 1000)
#define LIGHT_WHITE_LEVEL       48 /* per channel: full white at 255 is painful at arm's length */

static const char *TAG = "light";

static led_strip_handle_t s_strip;
static bool s_on;

static esp_err_t light_show(bool on)
{
    uint32_t level = on ? LIGHT_WHITE_LEVEL : 0;

    ESP_RETURN_ON_ERROR(led_strip_set_pixel(s_strip, 0, level, level, level), TAG, "set pixel");
    ESP_RETURN_ON_ERROR(led_strip_refresh(s_strip), TAG, "refresh");
    return ESP_OK;
}

esp_err_t light_init(void)
{
    led_strip_config_t strip_config = {
        .strip_gpio_num = LIGHT_LED_GPIO,
        .max_leds = LIGHT_LED_COUNT,
    };
    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = LIGHT_RMT_RESOLUTION_HZ,
    };

    ESP_RETURN_ON_ERROR(led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip), TAG, "create strip");
    s_on = false;
    ESP_RETURN_ON_ERROR(light_show(false), TAG, "show");
    printf("light: off\n");
    return ESP_OK;
}

esp_err_t light_set(bool on)
{
    ESP_RETURN_ON_ERROR(light_show(on), TAG, "show");
    if (on != s_on) {
        s_on = on;
        printf("light: %s\n", on ? "on" : "off");
    }
    return ESP_OK;
}

bool light_is_on(void)
{
    return s_on;
}
