// USB/IP settings, boot ownership and status. The decisions are usbip_core's (host tested); this file reads and writes
// the stored flag and calls the radio mux and the network side.
#include "usbip.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "netcfg.h"
#include "nvs.h"
#include "radio_mux.h"
#include "usbip_priv.h"

static const char *TAG = "usbip";

#define NVS_NAMESPACE "usbip"
#define NVS_KEY "enabled"

_Static_assert(USBIP_EVENTFDS == USBIP_EVENTFD_COUNT, "usbip.h and usbip_priv.h disagree on the eventfd count");

static atomic_bool s_enabled; // the stored flag (as read at boot, updated by usbip_set_enabled)
static atomic_bool s_active;  // USB/IP owns the port this boot
// Why USB/IP is enabled but not running: "<channel id> runs <mode>" (blocked) or, with s_start_failed,
// "USB/IP failed to start: <error>" (usbip_net_start failed at boot).
static char s_blocked_reason[96];
static bool s_start_failed;

// The stored flag; missing (or unreadable) means off.
static bool load_enabled(void)
{
    nvs_handle_t handle;
    uint8_t value = 0;

    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        if (err != ESP_ERR_NVS_NOT_FOUND) { // the namespace does not exist before the first write
            ESP_LOGW(TAG, "reading the enabled flag: nvs_open: %s, using off", esp_err_to_name(err));
        }
        return false;
    }
    err = nvs_get_u8(handle, NVS_KEY, &value);
    nvs_close(handle);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "reading the enabled flag: %s, using off", esp_err_to_name(err));
    }
    return err == ESP_OK && value != 0;
}

static esp_err_t store_enabled(bool on)
{
    nvs_handle_t handle;

    ESP_RETURN_ON_ERROR(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle), TAG, "nvs_open");
    esp_err_t err = nvs_set_u8(handle, NVS_KEY, on ? 1 : 0);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

// "<channel id> runs <mode>" for the stored USB channel holding otbr or ziggurat; false when none does.
static bool usb_stack_held(char *why, size_t cap)
{
    char id[RADIO_MUX_ID_MAX];
    radio_mux_mode_t mode;

    if (!radio_mux_stored_usb_stack(id, sizeof(id), &mode)) {
        return false;
    }
    if (why != NULL) {
        snprintf(why, cap, "%s runs %s", id, radio_mux_mode_name(mode));
    }
    return true;
}

esp_err_t usbip_boot(void)
{
    const bool enabled = load_enabled();

    atomic_store(&s_enabled, enabled);
    if (!enabled) {
        return ESP_OK; // the radio mux owns the port as before: nothing else runs
    }
    const bool held = usb_stack_held(s_blocked_reason, sizeof(s_blocked_reason));

    if (usbip_core_boot_state(enabled, held) == USBIP_STATE_BLOCKED) {
        ESP_LOGW(TAG, "enabled, but %s: the radio mux keeps the USB port", s_blocked_reason);
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(radio_mux_set_usb_external(RADIO_MUX_USB_EXTERNAL_REASON), TAG, "radio_mux_set_usb_external");
    const esp_err_t err = usbip_net_start();

    if (err != ESP_OK) {
        // The radio mux was told the port is external: it stays so this boot, and the status says why nothing runs.
        snprintf(s_blocked_reason, sizeof(s_blocked_reason), "USB/IP failed to start: %s", esp_err_to_name(err));
        s_start_failed = true;
        ESP_LOGE(TAG, "%s", s_blocked_reason);
        return err;
    }
    atomic_store(&s_active, true);
    ESP_LOGI(TAG, "the USB port is exported over USB/IP on TCP %u", (unsigned)USBIP_PORT);
    return ESP_OK;
}

bool usbip_active(void)
{
    return atomic_load(&s_active);
}

usbip_set_verdict_t usbip_set_enabled(bool on, bool long_op, char *detail, size_t cap)
{
    char why[96] = "";
    const bool held = on && usb_stack_held(why, sizeof(why));
    const usbip_set_verdict_t v = usbip_core_check_enable(on, atomic_load(&s_enabled), long_op, held);

    if (v == USBIP_SET_REFUSE_IN_USE && detail != NULL && cap > 0) {
        snprintf(detail, cap, "%s: move it or switch it off first", why);
    }
    if (v != USBIP_SET_REBOOT) {
        return v;
    }
    const esp_err_t err = store_enabled(on);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "storing the enabled flag: %s", esp_err_to_name(err));
        return USBIP_SET_REFUSE_BUSY;
    }
    atomic_store(&s_enabled, on);
    return v;
}

esp_err_t usbip_kick(void)
{
    ESP_RETURN_ON_FALSE(usbip_active(), ESP_ERR_INVALID_STATE, TAG, "not active");
    usbip_net_kick();
    return ESP_OK;
}

void usbip_get_status(usbip_status_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->enabled = atomic_load(&s_enabled);
    out->active = atomic_load(&s_active);
    // After a failed start the port is still taken from the radio mux this boot: only turning USB/IP off waits for a
    // reboot. Staying on is not a pending change (the reason and last_error say what failed).
    const bool failed = s_start_failed;

    out->pending_reboot = failed ? !out->enabled : out->enabled != out->active;
    out->port = USBIP_PORT;
    out->iface = netcfg_running_mode() == NETCFG_MODE_SPLIT ? "mgmt" : "single";
    if (out->active) {
        usbip_net_snapshot(out);
        return;
    }
    if (out->enabled && s_blocked_reason[0] != '\0' && !failed) {
        out->state = USBIP_STATE_BLOCKED;
        strlcpy(out->reason, s_blocked_reason, sizeof(out->reason));
    } else {
        out->state = USBIP_STATE_OFF; // off, enabled since boot (pending_reboot), or a failed start
        if (failed) {
            strlcpy(out->reason, s_blocked_reason, sizeof(out->reason));
            strlcpy(out->last_error, s_blocked_reason, sizeof(out->last_error));
        }
    }
}
