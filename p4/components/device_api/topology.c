#include <stdio.h>
#include <string.h>
#include "br_core_mode.h"
#include "device_api_priv.h"

static void set_reply(device_api_topology_reply_t *out, int status, const char *error, const char *detail)
{
    out->status = status;
    out->error = error;
    snprintf(out->detail, sizeof(out->detail), "%s", detail ? detail : "");
}

static void set_mode_reply(device_api_topology_reply_t *out, int mode)
{
    char detail[sizeof(out->detail)];

    snprintf(detail, sizeof(detail), "mode %s", device_api_mode_name(mode));
    set_reply(out, 409, "busy", detail);
}

void device_api_topology_start_reply(esp_err_t start_err, int mode, bool long_op_busy, const char *long_op_detail,
                                     bool pcap_busy, const char *pcap_detail, device_api_topology_reply_t *out)
{
    if (mode != BR_MODE_BR_RUNNING) {
        set_mode_reply(out, mode);
        return;
    }
    if (long_op_busy) {
        set_reply(out, 409, "busy", long_op_detail);
        return;
    }
    if (pcap_busy) {
        set_reply(out, 409, "busy", pcap_detail);
        return;
    }
    switch (start_err) {
    case ESP_OK:
    case ESP_ERR_INVALID_STATE: // a scan is already running: join it
        set_reply(out, 202, NULL, "");
        break;
    case ESP_ERR_NOT_ALLOWED: // the mode left BR_RUNNING before the start; `mode` was read after the refusal
        set_mode_reply(out, mode);
        break;
    case ESP_ERR_NOT_FINISHED:
        set_reply(out, 409, "busy", "mesh diagnostics are in use");
        break;
    case ESP_ERR_NO_MEM:
        set_reply(out, 500, "no_memory", "out of memory");
        break;
    case ESP_ERR_NOT_SUPPORTED: // mesh_scan was never initialised
        set_reply(out, 503, "busy", "topology scan unavailable");
        break;
    default: // ESP_ERR_TIMEOUT (OpenThread lock), ESP_FAIL (discover error)
        set_reply(out, 503, "busy", "OpenThread is busy, retry");
        break;
    }
}
