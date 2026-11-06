/*
 * The Zigbee node: an HA on/off light router, structured after esp-zigbee-sdk's on_off_light example (made a router: it
 * does not form a network, it steers into one). Console lines read by the pytest helper: `joined: yes` / `joined: no`
 * on every change of the join state.
 */

#include <stdio.h>

#include "esp_check.h"
#include "esp_timer.h"
#include "esp_zigbee.h"
#include "ezbee/zha.h"

#include "light.h"
#include "zigbee_node.h"

#define STEERING_RETRY_MS     1000
#define ALL_CHANNELS_MASK     0x07FFF800U /* channels 11..26 */
#define MANUFACTURER_NAME     "\x09""ESPRESSIF"
#define MODEL_IDENTIFIER      "\x0d""otbr_test_dev"
#define ZIGBEE_TASK_STACK     4096
#define ZIGBEE_TASK_PRIORITY  5

static const char *TAG = "zigbee";

static esp_timer_handle_t s_commissioning_timer;
static ezb_bdb_comm_mode_t s_commissioning_mode;
static volatile bool s_joined;

static void joined_set(bool joined)
{
    if (joined != s_joined) {
        s_joined = joined;
        printf("joined: %s\n", joined ? "yes" : "no");
    }
}

static void commissioning_timer_cb(void *arg)
{
    if (esp_zigbee_lock_acquire(portMAX_DELAY)) {
        (void)ezb_bdb_start_top_level_commissioning(s_commissioning_mode);
        esp_zigbee_lock_release();
    }
}

/* Called from the Zigbee task or the timer task: the timer task takes the Zigbee lock itself. */
static void commissioning_schedule(ezb_bdb_comm_mode_t mode, uint32_t delay_ms)
{
    s_commissioning_mode = mode;
    (void)esp_timer_stop(s_commissioning_timer);
    ESP_ERROR_CHECK(esp_timer_start_once(s_commissioning_timer, (uint64_t)delay_ms * 1000));
}

static bool app_signal_handler(const ezb_app_signal_t *app_signal)
{
    ezb_app_signal_type_t signal_type = ezb_app_signal_get_type(app_signal);

    switch (signal_type) {
    case EZB_ZDO_SIGNAL_SKIP_STARTUP:
        ESP_LOGI(TAG, "Initialize Zigbee stack");
        ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_INITIALIZATION);
        break;
    case EZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case EZB_BDB_SIGNAL_DEVICE_REBOOT: {
        ezb_bdb_comm_status_t status = *((ezb_bdb_comm_status_t *)ezb_app_signal_get_params(app_signal));
        if (status != EZB_BDB_STATUS_SUCCESS) {
            ESP_LOGW(TAG, "%s failed with status(0x%02x), retrying", ezb_app_signal_to_string(signal_type), status);
            commissioning_schedule(EZB_BDB_MODE_INITIALIZATION, STEERING_RETRY_MS);
        } else if (ezb_bdb_is_factory_new()) {
            ESP_LOGI(TAG, "Factory new, steering");
            ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_NETWORK_STEERING);
        } else {
            ESP_LOGI(TAG, "Rejoined the stored network (PAN 0x%04hx)", ezb_nwk_get_panid());
            joined_set(true);
        }
    } break;
    case EZB_BDB_SIGNAL_STEERING: {
        ezb_bdb_comm_status_t status = *((ezb_bdb_comm_status_t *)ezb_app_signal_get_params(app_signal));
        if (status == EZB_BDB_STATUS_SUCCESS) {
            ESP_LOGI(TAG, "Joined: PAN 0x%04hx, channel %d, short address 0x%04hx", ezb_nwk_get_panid(),
                     ezb_nwk_get_current_channel(), ezb_nwk_get_short_address());
            joined_set(true);
        } else {
            ESP_LOGW(TAG, "Steering failed with status(0x%02x), retrying", status);
            commissioning_schedule(EZB_BDB_MODE_NETWORK_STEERING, STEERING_RETRY_MS);
        }
    } break;
    case EZB_ZDO_SIGNAL_LEAVE: {
        const ezb_zdo_signal_leave_params_t *params = ezb_app_signal_get_params(app_signal);
        ESP_LOGI(TAG, "Left the network (type %d)", params->leave_type);
        joined_set(false);
        if (params->leave_type == EZB_ZDO_LEAVE_TYPE_RESET) {
            (void)light_set(false);
            commissioning_schedule(EZB_BDB_MODE_NETWORK_STEERING, STEERING_RETRY_MS);
        }
    } break;
    default:
        ESP_LOGI(TAG, "Zigbee signal: %s(0x%02x)", ezb_app_signal_to_string(signal_type), signal_type);
        break;
    }
    return true;
}

static void set_attr_value_handler(ezb_zcl_set_attr_value_message_t *message)
{
    ESP_RETURN_ON_FALSE(message,, TAG, "message is empty");
    const ezb_zcl_attribute_t *attribute = &message->in.attribute;

    /* Only the OnOff attribute (a bool): OnTime, OffWaitTime and GlobalSceneControl are other attributes of the cluster. */
    if (message->info.cluster_id == EZB_ZCL_CLUSTER_ID_ON_OFF &&
            message->info.status == EZB_ZCL_STATUS_SUCCESS &&
            attribute->id == EZB_ZCL_ATTR_ON_OFF_ON_OFF_ID &&
            attribute->data.type == EZB_ZCL_ATTR_TYPE_BOOL &&
            attribute->data.value != NULL) {
        (void)light_set(*(uint8_t *)attribute->data.value != 0);
    }
}

static void zcl_action_handler(ezb_zcl_core_action_callback_id_t callback_id, void *message)
{
    if (callback_id == EZB_ZCL_CORE_SET_ATTR_VALUE_CB_ID) {
        set_attr_value_handler(message);
    }
}

static esp_err_t light_device_create(void)
{
    ezb_af_device_desc_t dev_desc = ezb_af_create_device_desc();
    ezb_zha_on_off_light_config_t light_cfg = EZB_ZHA_ON_OFF_LIGHT_CONFIG();
    ezb_af_ep_desc_t ep_desc = ezb_zha_create_on_off_light(ZIGBEE_NODE_ON_OFF_LIGHT_EP, &light_cfg);
    ezb_zcl_cluster_desc_t basic_desc =
        ezb_af_endpoint_get_cluster_desc(ep_desc, EZB_ZCL_CLUSTER_ID_BASIC, EZB_ZCL_CLUSTER_SERVER);

    ezb_zcl_basic_cluster_desc_add_attr(basic_desc, EZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID, (void *)MANUFACTURER_NAME);
    ezb_zcl_basic_cluster_desc_add_attr(basic_desc, EZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID, (void *)MODEL_IDENTIFIER);
    ESP_RETURN_ON_ERROR(ezb_af_device_add_endpoint_desc(dev_desc, ep_desc), TAG, "add endpoint");
    ESP_RETURN_ON_ERROR(ezb_af_device_desc_register(dev_desc), TAG, "register device");
    ezb_zcl_core_action_handler_register(zcl_action_handler);
    return ESP_OK;
}

static void zigbee_main_task(void *arg)
{
    esp_zigbee_config_t config = {
        .device_config = {
            .device_type = EZB_NWK_DEVICE_TYPE_ROUTER,
            .install_code_policy = false,
            .zczr_config = {
                .max_children = 10,
            },
        },
        .platform_config = {
            .storage_partition_name = "nvs",
            .radio_config = {
                .radio_mode = ESP_ZIGBEE_RADIO_MODE_NATIVE,
            },
        },
    };

    ESP_ERROR_CHECK(esp_zigbee_init(&config));
    ezb_aps_secur_enable_distributed_security(false);
    ESP_ERROR_CHECK(ezb_bdb_set_primary_channel_set(ALL_CHANNELS_MASK));
    ESP_ERROR_CHECK(ezb_bdb_set_secondary_channel_set(ALL_CHANNELS_MASK));
    ESP_ERROR_CHECK(ezb_app_signal_add_handler(app_signal_handler));
    ESP_ERROR_CHECK(light_device_create());
    ESP_ERROR_CHECK(esp_zigbee_start(false));
    esp_zigbee_launch_mainloop();

    esp_zigbee_deinit();
    vTaskDelete(NULL);
}

esp_err_t zigbee_node_start(void)
{
    const esp_timer_create_args_t timer_args = {
        .callback = commissioning_timer_cb,
        .name = "zb_commission",
    };

    ESP_RETURN_ON_ERROR(esp_timer_create(&timer_args, &s_commissioning_timer), TAG, "create timer");
    ESP_RETURN_ON_FALSE(xTaskCreate(zigbee_main_task, "zigbee_main", ZIGBEE_TASK_STACK, NULL, ZIGBEE_TASK_PRIORITY, NULL) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "create task");
    return ESP_OK;
}

esp_err_t zigbee_node_factory_reset(void)
{
    ESP_RETURN_ON_FALSE(esp_zigbee_lock_acquire(pdMS_TO_TICKS(1000)), ESP_ERR_TIMEOUT, TAG, "zigbee lock");
    if (ezb_bdb_is_factory_new()) {
        (void)light_set(false);
        commissioning_schedule(EZB_BDB_MODE_NETWORK_STEERING, 0);
    } else {
        ezb_bdb_reset_via_local_action(); /* the LEAVE signal restarts steering */
    }
    esp_zigbee_lock_release();
    return ESP_OK;
}

bool zigbee_node_is_joined(void)
{
    return s_joined;
}
