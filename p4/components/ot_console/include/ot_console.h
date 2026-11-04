#pragma once

// Thread console: the board's OpenThread CLI for one network session at a time (the WebSocket transport in device_api)
// and, in test builds, the USB console's `ot` command. Owns the setting (NVS namespace `console`, key `enabled`, off
// by default), the CLI's output callback and the input path.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OT_CONSOLE_CLOSE_TAKEN_OVER 4001
#define OT_CONSOLE_CLOSE_TURNED_OFF 4003
#define OT_CONSOLE_CLOSE_STALLED 4009

/** The transport's side of a session. */
typedef struct {
    /** Output or a notice is waiting (ot_console_take). Called from the OpenThread task holding the OpenThread lock:
     *  must not block and must not call lwIP (its thread may be waiting for that lock). */
    void (*wake)(void *ctx);
    /** The session ended from this side (take-over, turned off): send the notice, then close with the code. */
    void (*close)(void *ctx, uint16_t code, const char *notice);
    void *ctx;
} ot_console_sink_t;

/** Load the setting and register the br_core CLI hook. Call before br_core_start. */
esp_err_t ot_console_init(void);

/** The stored setting; false while ot_console_init has not succeeded. */
bool ot_console_enabled(void);

/**
 * Store the setting; turning it off closes the session with 4003 and the off notice. ESP_ERR_INVALID_STATE when
 * ot_console_init has not succeeded.
 */
esp_err_t ot_console_set_enabled(bool on);

/**
 * Start a session for `peer` (an address, shown to others). An earlier session is taken over: its sink gets
 * close(4001, taken-over notice). The new session's first output is the Connected or the Thread-not-running notice.
 * @return ESP_ERR_INVALID_STATE when the console is off
 */
esp_err_t ot_console_attach(const char *peer, ot_console_sink_t sink, uint32_t *session_id);

/** End a session (the transport's socket closed). Unknown or old ids are ignored. */
void ot_console_detach(uint32_t session_id);

/**
 * Run a line from the session. Refusals (too long, Busy, Thread not running) become notices in the session's output.
 * @return ESP_ERR_INVALID_STATE when the id is not the current session
 */
esp_err_t ot_console_submit(uint32_t session_id, const char *line);

/**
 * Drain the session's output into out: each call returns one frame's worth, either one notice (a pending drop count
 * first, as the dropped notice) or CLI output. 0 when nothing waits or the id is not the current session.
 */
size_t ot_console_take(uint32_t session_id, char *out, size_t cap);

/** Bytes of output waiting for the session (0 for an old id). */
size_t ot_console_queued(uint32_t session_id);

/** Is a session attached? When it is, its peer and its age in seconds (either may be NULL). */
bool ot_console_session(char *peer, size_t peer_cap, uint32_t *since_s);

#ifdef OTBR_TEST_BUILD
/** Test builds: register the USB console's `ot` command (call once the console is initialised). */
esp_err_t ot_console_usb_command_register(void);
#endif

#ifdef __cplusplus
}
#endif
