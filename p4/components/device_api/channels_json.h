#pragma once

// /device/channels bodies and decisions (spec docs/superpowers/specs/2026-11-06-radio-channel-mux-design.md, section
// API). Pure: host-tested in test_host/. Apart from device_api_priv.h so that the components that include that header
// by path do not need radio_mux.

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "device_api_priv.h"
#include "radio_mux.h"

#ifdef __cplusplus
extern "C" {
#endif

// The GET body of RADIO_MUX_CHANNELS_MAX channels with every text at its longest (escaped); a host test checks it.
#define DEVICE_API_CHANNELS_BODY_MAX 12288
// One channel (the PUT reply), with "reboot".
#define DEVICE_API_CHANNEL_BODY_MAX 1536
// A PUT body: every field with the longest host fits several times over.
#define DEVICE_API_CHANNEL_PUT_MAX 512

/**
 * The GET /device/channels body: {"channels":[<channel>...]} in snapshot order. A channel is
 * {"id","kind","label","mode","stored_mode","state","error","line":{"baud","flow_control"},
 *  "usb":{"vid","pid","serial","driver"}|null,"caps":{"firmware_update","hw_reset","flow_control"},
 *  "rcp_proxy":{"direction","adapter","port","host","framing","state","peer"}|null,
 *  "firmware":{"type","version","chip","baud","source","at"}|null,
 *  "counters":{"frames_in","frames_out","bytes_in","bytes_out","drops","resets","rx_errors"}}.
 * kind "c6" or "usb"; error, usb.serial, usb.driver, rcp_proxy.state and .peer, firmware.version, .chip, .baud (0)
 * and .at (0: the clock was not set) are null when empty; usb.vid and .pid are 4 lowercase hex digits; usb is null for
 * the C6; rcp_proxy (the stored settings) is null for a USB device without a driver, where no mode runs; firmware is
 * null without a record. Returns the length, or -1 when `cap` is too small.
 */
int device_api_channels_json(char *buf, size_t cap, const radio_mux_snapshot_t *s);

/** One channel object as in device_api_channels_json; with `reboot`, ,"reboot":true closes it (the PUT reply). */
int device_api_channel_json(char *buf, size_t cap, const radio_mux_chan_info_t *c, bool reboot);

/** The 202 body of a move: {"moving":true,"moved_from":"<id>"} ("" or NULL: null). */
int device_api_channel_moving_json(char *buf, size_t cap, const char *moved_from);

/**
 * Parse a PUT /device/channels/{id} body: an object with any of "mode" (a settable mode name: not "unavailable"),
 * "baud" (an accepted line speed), "flow_control" (boolean), "rcp_proxy" (an object with any of "direction" listen|
 * connect, "adapter" tcp|udp, "port" 1..65535, "host" a string of at most 63 bytes, "framing" hdlc|raw, and "kick"
 * (boolean, never stored)). Fields not given are not set (has_* false, their proxy_fields bit clear, the proxy values
 * the defaults). ESP_ERR_INVALID_ARG with *bad_field naming the field ("body" for a body that is not a JSON object,
 * "rcp_proxy.port" for a nested one, the key itself for an unknown key); *bad_field NULL on ESP_OK.
 */
esp_err_t device_api_channel_put_parse(const char *body, radio_mux_set_req_t *out, const char **bad_field);

/**
 * rcp_proxy_settings_check on `stored` with the request's given settings over it: NULL when the request gives none or
 * the merged settings pass, else the field to name in the 400 ("rcp_proxy.port", "rcp_proxy.host").
 */
const char *device_api_channel_proxy_refusal(const radio_mux_proxy_settings_t *stored, const radio_mux_set_req_t *req);

/**
 * HTTP status and error code of a radio_mux_set verdict: APPLY and APPLY_REBOOT 200, MOVE 202 (code NULL); 400
 * bad_value; 404 unknown_channel; 409 unavailable, not_available, incompatible, mode_taken, hybrid_not_supported,
 * probe_failed, busy; 500 failed (also for a value out of range).
 */
void device_api_channel_refusal(radio_mux_verdict_t v, int *status, const char **code);

/** Default detail of a refusal (used when the mux gave no reason); "" for an accepted verdict. */
const char *device_api_channel_refusal_detail(radio_mux_verdict_t v);

/**
 * Reply of DELETE /device/channels/{id} from radio_mux_forget and radio_mux_started: ESP_OK 204; ESP_ERR_NOT_FOUND 404
 * unknown_channel; ESP_ERR_INVALID_STATE 409 in_use (the C6, or a radio that is plugged in), or 503 not_started when
 * the mux is not running; anything else 500 failed.
 */
device_api_error_t device_api_channel_forget_error(esp_err_t err, bool mux_started);

/**
 * Whether the PUT moves OpenThread (and so takes the long-operation slot first): it sets mode otbr on `id` while
 * another channel (`otbr_id`, radio_mux_otbr_channel) has it.
 */
bool device_api_channel_is_move(const radio_mux_set_req_t *req, const char *id, const char *otbr_id);

/**
 * A move of Thread (device_api_channel_is_move) goes alone: with baud, flow_control or any rcp_proxy setting (kick
 * included) the PUT is refused, 400 bad_value with the returned detail, before anything is claimed or stored (the
 * mux would refuse them on the leased target after the 202). NULL when the PUT may go on.
 */
const char *device_api_channel_move_refusal(const radio_mux_set_req_t *req, const char *id, const char *otbr_id);

/**
 * While a move of Thread runs (`move_running`: br_core_move_running, or the long-operation slot held by RADIO_MOVE), a
 * mode PUT on any channel is refused, 409 busy with the returned detail. NULL when the PUT may go on.
 */
const char *device_api_channel_move_busy(const radio_mux_set_req_t *req, bool move_running);

/**
 * Whether the PUT retries OpenThread's failed start (br_core_retry_otbr): an accepted (APPLY) PUT of mode otbr on the
 * OTBR channel `otbr_id` while the board `mode` (br_core_mode_t) is RADIO_FAULT.
 */
bool device_api_channel_retry_otbr(const radio_mux_set_req_t *req, radio_mux_verdict_t v, const char *id,
                                   const char *otbr_id, int mode);

/**
 * The board's view over the mux's: while the board `mode` (br_core_mode_t) is RADIO_FAULT, the OTBR channel `otbr_id`
 * that the mux lists running (its consumer is attached, but OpenThread did not start on it) shows state fault with
 * `fault_reason`. Nothing else changes.
 */
void device_api_channels_overlay_otbr(radio_mux_snapshot_t *s, const char *otbr_id, int mode, const char *fault_reason);

#ifdef __cplusplus
}
#endif
