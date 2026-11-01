#pragma once

// Pure helpers of device_api (no esp_http_server, no OpenThread): host-tested in test_host/.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- json_out.c: bounded JSON writer ---- */

typedef struct {
    char *buf;
    size_t cap;
    size_t len;
    bool overflow; /**< the output did not fit; buf holds a truncated, NUL-terminated prefix */
} json_out_t;

void json_out_init(json_out_t *w, char *buf, size_t cap);
/** Append raw text (keys, punctuation). */
void json_out_raw(json_out_t *w, const char *text);
/** Append a quoted, escaped string; NULL writes null. */
void json_out_str(json_out_t *w, const char *text);
void json_out_uint(json_out_t *w, uint32_t value);
void json_out_int(json_out_t *w, int32_t value);
void json_out_bool(json_out_t *w, bool value);

/* ---- json_err.c: error bodies and esp_err mapping ---- */

typedef struct {
    int http_status;
    const char *code; /**< short machine-readable error code */
    const char *detail; /**< default human-readable detail, NULL when the caller supplies one */
} device_api_error_t;

/** `{"error": code, "detail": detail}`. Returns the length, or -1 when it does not fit `cap` (nothing is truncated). */
int device_api_error_json(char *buf, size_t cap, const char *code, const char *detail);

/** Status line text for the codes device_api sends, e.g. 409 -> "409 Conflict". */
const char *device_api_status_line(int http_status);

/** Error of a bundle update (esp_br_ota_from_httpd, esp_br_http_ota). */
device_api_error_t device_api_map_ota_error(esp_err_t err);

/**
 * GET /device/sniffer.pcap without a channel: the capture uses the channel of the active dataset. http_status 0 = go on;
 * 503 "busy" when the OpenThread state could not be read (retry; the dataset is unknown, not absent); 400 "no_channel"
 * when it was read and there is no dataset.
 */
device_api_error_t device_api_pcap_default_channel_error(bool ot_state_valid, bool has_dataset);

/** Error of br_core_push_radio_firmware. */
device_api_error_t device_api_map_push_error(esp_err_t err);

/** Error of br_core_rom_recover: ESP_ERR_NOT_SUPPORTED (no jumper) 412, ESP_ERR_INVALID_STATE 409, anything else 502. */
device_api_error_t device_api_map_recover_error(esp_err_t err);

/**
 * Whole reply of POST /device/radio/recover from the result of br_core_rom_recover and the state after the restart.
 * `err` ESP_OK: *http_status 200 and `{"recovered":true,"rcp":<installed version | "no rcp firmware" | "unknown">,"mode":<mode name>,
 * "detail":<text>}` (BR_RUNNING: the RCP in its slot runs; otherwise `fault_reason` and, without RCP firmware, the upload hint; the restart finding no RCP firmware is not a failure of the recovery); otherwise the mapped error and its
 * `{"error","detail"}` body (`err_detail` replaces the default detail when not empty). Returns the body length, -1 if it does not fit.
 */
int device_api_recover_reply(esp_err_t err, const char *err_detail, int mode, const char *fault_reason, bool rcp_known,
                             const char *rcp_installed, char *buf, size_t cap, int *http_status);

/** Error of netcfg_set_trial (PUT /device/network): ESP_ERR_INVALID_ARG 400 "invalid_config" (the caller puts the field in detail). */
device_api_error_t device_api_map_network_set_error(esp_err_t err);

/** Error of netcfg_confirm: ESP_ERR_INVALID_STATE 409 "no_trial", ESP_ERR_NOT_ALLOWED 403 "wrong_interface". */
device_api_error_t device_api_map_network_confirm_error(esp_err_t err);

/* ---- limits.c: request validation before anything destructive happens ---- */

/** Content-Length check: 0 = acceptable, 400 for a missing/zero length, 413 for more than `max` bytes. */
int device_api_length_check(size_t len, size_t max);

/**
 * Check a from_url / ota url: error.http_status 0 = acceptable (http://), 400 "https_not_supported" (no certificate bundle
 * is built in) or 400 "invalid_request" (empty, other scheme).
 */
device_api_error_t device_api_check_url(const char *url);

/** `{"installed": "<version>"}` or `{"installed": null}` when `known` is false or the version is empty. -1 if it does not fit. */
int device_api_installed_json(char *buf, size_t cap, bool known, const char *version);

/* ---- query.c: query string parameters ---- */

/**
 * Value of `key` in a raw URL query ("a=1&b=x%20y"), percent-decoded ('+' stays '+'), NUL-terminated in `out`.
 * ESP_ERR_NOT_FOUND without the key, ESP_ERR_INVALID_SIZE if the value does not fit `cap`, ESP_ERR_INVALID_ARG for a
 * bad percent escape or NULL arguments. A key without '=' has the empty value.
 */
esp_err_t device_api_query_value(const char *query, const char *key, char *out, size_t cap);

/** As device_api_query_value, parsed as a decimal number in [min, max]; ESP_ERR_INVALID_ARG if it is not one. */
esp_err_t device_api_query_uint(const char *query, const char *key, uint32_t min, uint32_t max, uint32_t *out);

/* ---- long_op.c: one long operation at a time ---- */

typedef enum {
    DEVICE_API_OP_OTA,        /**< bundle update (upload or URL) */
    DEVICE_API_OP_RADIO_PUSH, /**< RCP firmware push */
    DEVICE_API_OP_ROM_RECOVER, /**< ROM recovery of the C6 base image; shares the slot with OTA and the push */
    DEVICE_API_OP_PCAP,       /**< pcap stream; independent of the other two */
} device_api_op_t;

/** True and the operation is recorded when nothing conflicts: OTA, RADIO_PUSH and ROM_RECOVER exclude each other and themselves. */
bool device_api_op_try_begin(device_api_op_t op);
/** True while an OTA or a firmware push holds the shared slot (a successful OTA keeps it until the reboot). */
bool device_api_op_long_running(void);
/** Release the operation. An OTA that succeeded is deliberately never released: the update waits for its reboot. */
void device_api_op_end(device_api_op_t op);
/** Text for the 409 that follows a refused device_api_op_try_begin(op). */
const char *device_api_op_busy_detail(device_api_op_t op);

/* ---- status_json.c: GET /device/status ---- */

/** pd_state of a status whose OpenThread part could not be read (the OpenThread lock was busy): pd.state is null. */
#define DEVICE_API_PD_STATE_UNKNOWN (-2)

#define DEVICE_API_MAX_IPV6 6
#define DEVICE_API_ADDR_LEN 48

typedef struct {
    int mode;                   /**< br_core_mode_t */
    const char *app_version;
    int app_slot;
    bool rcp_installed_known;   /**< false: the probe failed, rcp.installed is null */
    const char *rcp_installed;  /**< "" = no valid RCP firmware */
    bool rcp_compatible;
    bool jumper_present;
    bool jumper_recovery_failed; /**< a ROM recovery failed and none worked since */
    const char *base_expected;
    const char *base_installed;
    const char *ipv4;           /**< "" = none */
    char ipv6[DEVICE_API_MAX_IPV6][DEVICE_API_ADDR_LEN];
    size_t ipv6_count;
    int pd_state;               /**< otBorderRoutingDhcp6PdState; -1 = border router not initialised; DEVICE_API_PD_STATE_UNKNOWN = not read (JSON null) */
    const char *pd_prefix;
    bool sniffer_active;
    bool sniffer_ended_by_fault;
    uint8_t sniffer_channel;
    uint32_t sniffer_frames;
    uint32_t sniffer_drops;
    uint32_t sniffer_elapsed_s;
    uint32_t sniffer_duration_s;
    const char *fault_reason;   /**< "" = none (JSON null) */
    bool storage_ok;            /**< false: NVS did not initialise (degraded boot) */
} device_api_status_t;

/** Name of a br_core_mode_t: "STARTING", "BR_RUNNING", "SNIFFER", "RADIO_FAULT" ("UNKNOWN" otherwise). */
const char *device_api_mode_name(int mode);

/** The /device/status body. Returns its length, or -1 when `cap` is too small. */
int device_api_status_json(char *buf, size_t cap, const device_api_status_t *status);

#ifdef __cplusplus
}
#endif
