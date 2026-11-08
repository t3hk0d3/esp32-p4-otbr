#include "device_api_priv.h"
#include <stdio.h>
#include "br_core_mode.h"
#include "ota_bundle.h"
#include "rcp_sync.h"

#define OTA_ERR_BASE 0x1500   // ESP_ERR_OTA_BASE (esp_ota_ops.h): esp_ota_end's image validation errors
#define OTA_ERR_LAST 0x15ff
#define IMAGE_ERR_BASE 0x2000 // ESP_ERR_IMAGE_BASE (esp_image_format.h)
#define IMAGE_ERR_LAST 0x20ff

int device_api_error_json(char *buf, size_t cap, const char *code, const char *detail)
{
    json_out_t w;

    json_out_init(&w, buf, cap);
    json_out_raw(&w, "{\"error\":");
    json_out_str(&w, code);
    json_out_raw(&w, ",\"detail\":");
    json_out_str(&w, detail ? detail : "");
    json_out_raw(&w, "}");
    return w.overflow ? -1 : (int)w.len;
}

const char *device_api_status_line(int http_status)
{
    switch (http_status) {
    case 200:
        return "200 OK";
    case 202:
        return "202 Accepted";
    case 204:
        return "204 No Content";
    case 400:
        return "400 Bad Request";
    case 403:
        return "403 Forbidden";
    case 412:
        return "412 Precondition Failed";
    case 413:
        return "413 Payload Too Large";
    case 404:
        return "404 Not Found";
    case 409:
        return "409 Conflict";
    case 500:
        return "500 Internal Server Error";
    case 502:
        return "502 Bad Gateway";
    case 503:
        return "503 Service Unavailable";
    case 504:
        return "504 Gateway Timeout";
    default:
        return "500 Internal Server Error";
    }
}

static bool in_range(esp_err_t err, int first, int last)
{
    return err >= first && err <= last;
}

device_api_error_t device_api_map_ota_error(esp_err_t err)
{
    if (err == ESP_BR_OTA_ERR_NOT_CONFIRMED) {
        return (device_api_error_t) {
            409, "busy", "the running update is not confirmed yet; retry in up to 60 s"
        };
    }
    if (err == ESP_ERR_INVALID_STATE) {
        return (device_api_error_t) {
            409, "busy", NULL
        };
    }
    if (err == ESP_ERR_NO_MEM) {
        return (device_api_error_t) {
            500, "no_memory", NULL
        };
    }
    if (err == ESP_ERR_INVALID_CRC || err == ESP_ERR_INVALID_VERSION || err == ESP_ERR_INVALID_SIZE ||
            in_range(err, OTA_ERR_BASE, OTA_ERR_LAST) || in_range(err, IMAGE_ERR_BASE, IMAGE_ERR_LAST)) {
        return (device_api_error_t) {
            400, "invalid_bundle", NULL
        };
    }
    if (err == ESP_ERR_INVALID_ARG) {
        return (device_api_error_t) {
            400, "invalid_request", NULL
        };
    }
    return (device_api_error_t) {
        502, "transfer_failed", NULL
    };
}

device_api_error_t device_api_pcap_default_channel_error(bool ot_state_valid, bool has_dataset)
{
    if (!ot_state_valid) {
        return (device_api_error_t) {
            503, "busy", "OpenThread is busy; retry"
        };
    }
    if (!has_dataset) {
        return (device_api_error_t) {
            400, "no_channel", "no channel given and no active dataset"
        };
    }
    return (device_api_error_t) {
        0, NULL, NULL
    };
}

// A C6 job refused by the radio mux: rcp-proxy on the C6 has a peer.
static const device_api_error_t C6_BUSY = {409, "c6_busy", "rcp-proxy on the C6 has a connected peer"};

device_api_error_t device_api_map_push_error(esp_err_t err)
{
    if (err == RCP_SYNC_ERR_C6_BUSY) {
        return C6_BUSY;
    }
    if (err == ESP_ERR_INVALID_STATE) {
        return (device_api_error_t) {
            409, "busy", NULL
        };
    }
    if (err == ESP_ERR_INVALID_SIZE) {
        return (device_api_error_t) {
            400, "image_too_large", NULL
        };
    }
    if (err == ESP_ERR_INVALID_ARG) {
        return (device_api_error_t) {
            400, "invalid_request", NULL
        };
    }
    if (err == ESP_ERR_NO_MEM) {
        return (device_api_error_t) {
            500, "no_memory", NULL
        };
    }
    if (err == RCP_SYNC_ERR_IMAGE_REJECTED) {
        return (device_api_error_t) {
            400, "invalid_image", NULL
        };
    }
    if (err == RCP_SYNC_ERR_HOSTED_OTA) {
        return (device_api_error_t) {
            502, "install_failed", NULL
        };
    }
    if (err == RCP_SYNC_ERR_RESTART) {
        return (device_api_error_t) {
            502, "restart_failed", "the image is installed, but OpenThread did not start on it"
        };
    }
    return (device_api_error_t) {
        502, "push_failed", NULL
    };
}

device_api_error_t device_api_map_recover_error(esp_err_t err)
{
    if (err == ESP_ERR_NOT_SUPPORTED) {
        return (device_api_error_t) {
            412, "no_jumper", "the maintenance jumper is not detected"
        };
    }
    if (err == RCP_SYNC_ERR_C6_BUSY) {
        return C6_BUSY;
    }
    if (err == ESP_ERR_INVALID_STATE) {
        return (device_api_error_t) {
            409, "busy", NULL
        };
    }
    if (err == ESP_ERR_NO_MEM) {
        return (device_api_error_t) {
            500, "no_memory", NULL
        };
    }
    return (device_api_error_t) {
        502, "recovery_failed", NULL
    };
}

const char *device_api_radio_job_busy(int mode, bool thread_on_c6)
{
    if (!thread_on_c6) {
        return NULL;
    }
    if (mode == BR_MODE_SNIFFER) {
        return "a capture is running";
    }
    return mode == BR_MODE_STARTING ? "the border router is starting" : NULL;
}

int device_api_recover_reply(esp_err_t err, const char *err_detail, int mode, bool thread_on_c6, const char *fault_reason,
                             bool rcp_known, const char *rcp_installed, char *buf, size_t cap, int *http_status)
{
    if (err != ESP_OK) {
        const device_api_error_t error = device_api_map_recover_error(err);
        const char *detail = err_detail && err_detail[0] ? err_detail : (error.detail ? error.detail : "");

        *http_status = error.http_status;
        return device_api_error_json(buf, cap, error.code, detail);
    }
    const bool has_rcp = rcp_known && rcp_installed && rcp_installed[0];
    const char *rcp = has_rcp ? rcp_installed : (rcp_known ? "no rcp firmware" : "unknown");
    char detail[160];
    json_out_t w;

    if (thread_on_c6 && mode == BR_MODE_BR_RUNNING) {
        snprintf(detail, sizeof(detail), "C6 base image rewritten; the RCP firmware in its slot is running");
    } else if (!has_rcp) {
        snprintf(detail, sizeof(detail), "C6 base image rewritten; no RCP firmware: upload c6_rcp.bin");
    } else if (!thread_on_c6) { // the board mode and its fault are another channel's
        snprintf(detail, sizeof(detail), "C6 base image rewritten; the RCP firmware in its slot is kept (OpenThread "
                 "runs on another radio)");
    } else {
        snprintf(detail, sizeof(detail), "C6 base image rewritten; the border router is not running (%s)",
                 fault_reason && fault_reason[0] ? fault_reason : "see /device/status");
    }
    *http_status = 200;
    json_out_init(&w, buf, cap);
    json_out_raw(&w, "{\"recovered\":true,\"rcp\":");
    json_out_str(&w, rcp);
    json_out_raw(&w, ",\"mode\":");
    json_out_str(&w, device_api_mode_name(mode));
    json_out_raw(&w, ",\"detail\":");
    json_out_str(&w, detail);
    json_out_raw(&w, "}");
    return w.overflow ? -1 : (int)w.len;
}

device_api_error_t device_api_map_network_set_error(esp_err_t err)
{
    if (err == ESP_ERR_INVALID_ARG) {
        return (device_api_error_t) {
            400, "invalid_config", NULL
        };
    }
    if (err == ESP_ERR_INVALID_STATE) {
        return (device_api_error_t) {
            503, "not_started", "the network configuration is not running"
        };
    }
    if (err == ESP_ERR_NO_MEM) {
        return (device_api_error_t) {
            500, "no_memory", NULL
        };
    }
    return (device_api_error_t) {
        500, "store_failed", "the configuration could not be stored"
    };
}

device_api_error_t device_api_map_network_confirm_error(esp_err_t err)
{
    if (err == ESP_ERR_INVALID_STATE) {
        return (device_api_error_t) {
            409, "no_trial", "no network configuration is waiting for a confirm"
        };
    }
    if (err == ESP_ERR_NOT_ALLOWED) {
        return (device_api_error_t) {
            403, "wrong_interface", "confirm on the management interface of the new configuration"
        };
    }
    if (err == ESP_ERR_NO_MEM) {
        return (device_api_error_t) {
            500, "no_memory", NULL
        };
    }
    return (device_api_error_t) {
        500, "store_failed", "the configuration could not be stored"
    };
}
