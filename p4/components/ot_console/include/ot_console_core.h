#pragma once

// Decisions of the Thread console: line rules, which input may reach the OpenThread CLI, where each piece of CLI
// output goes, the bounded output buffer and the notice texts. Pure; host-tested in test_host/.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Room for a pending line: the largest OPENTHREAD_CONFIG_CLI_MAX_LINE_LENGTH any build uses. */
#define OT_CONSOLE_PENDING_LEN 640

typedef enum {
    OT_CONSOLE_LINE_OK,
    OT_CONSOLE_LINE_EMPTY,
    OT_CONSOLE_LINE_TOO_LONG,
} ot_console_line_t;

typedef enum {
    OT_CONSOLE_SRC_WS,  /**< the WebSocket session */
    OT_CONSOLE_SRC_USB, /**< the USB console's `ot` command */
} ot_console_src_t;

typedef enum {
    OT_CONSOLE_SEND,        /**< post the line to the CLI */
    OT_CONSOLE_BUSY,        /**< a command is still pending */
    OT_CONSOLE_NOT_RUNNING, /**< Thread is not running */
    OT_CONSOLE_IN_USE,      /**< USB line while a WebSocket session is attached */
} ot_console_verdict_t;

typedef enum {
    OT_CONSOLE_TO_SESSION,
    OT_CONSOLE_TO_STDOUT,
    OT_CONSOLE_DROP_PROMPT, /**< the CLI's "> " prompt: not forwarded */
    OT_CONSOLE_DROP,        /**< nobody to read it (no session, or the line's session left) */
} ot_console_dest_t;

/** What a session's transport gets next. */
typedef enum {
    OT_CONSOLE_TAKE_NONE,
    OT_CONSOLE_TAKE_GREETING, /**< the first queued notice, before anything else */
    OT_CONSOLE_TAKE_DROPPED,  /**< the "Output dropped" notice */
    OT_CONSOLE_TAKE_OUTPUT,
    OT_CONSOLE_TAKE_NOTICE,
} ot_console_take_t;

typedef struct ot_console_arb {
    char pending[OT_CONSOLE_PENDING_LEN]; /**< the line waiting for its prompt, "" when none */
    ot_console_src_t pending_src;
    bool has_pending;
    bool attached; /**< a WebSocket session is attached */
    bool running;  /**< the Thread stack is running */
    bool carry;    /**< the last write ended in '>', held back until the next write */
} ot_console_arb_t;

/** Bytes of a write to forward, and whether a held-back '>' goes out before them (to the same destination). */
typedef struct {
    size_t len;
    bool flush_carry;
} ot_console_span_t;

typedef struct {
    uint8_t *data;
    size_t cap, head, len;
    size_t dropped;
} ot_console_buf_t;

/**
 * Normalise a line in place: strip trailing CR/LF and surrounding blanks, then a leading "ot " and the blanks after
 * it. EMPTY when nothing is left, TOO_LONG when more than max_len bytes are left.
 */
ot_console_line_t ot_console_line_normalise(char *line, size_t max_len);

void ot_console_arb_init(ot_console_arb_t *a);

/** May this (normalised) line be posted? On SEND it becomes the pending line, except "factoryreset" while pending. */
ot_console_verdict_t ot_console_arb_submit(ot_console_arb_t *a, ot_console_src_t src, const char *line);

/**
 * Where a write of CLI output goes. `span` says how many leading bytes of `text` to forward (a trailing '>' is held
 * back) and whether a '>' held back from the previous write goes out first. The prompt clears the pending line.
 */
ot_console_dest_t ot_console_arb_output(ot_console_arb_t *a, const char *text, size_t len, ot_console_span_t *span);

void ot_console_arb_attach(ot_console_arb_t *a, bool attached);

/** Posting `line` failed: forget it if it is the pending line (a factoryreset sent past a pending line is not). */
void ot_console_arb_cancel(ot_console_arb_t *a, const char *line);

/** The stack started or stopped: either way no command is pending any more. */
void ot_console_arb_stack(ot_console_arb_t *a, bool running);

/** The pending line, NULL when none. */
const char *ot_console_arb_pending(const ot_console_arb_t *a);

/** Order of a session's output: the greeting first, then dropped bytes, output, notices. */
ot_console_take_t ot_console_take_next(bool greeted, size_t notices, size_t dropped, size_t buffered);

void ot_console_buf_init(ot_console_buf_t *b, uint8_t *storage, size_t cap);

/** Append; past the cap the oldest bytes are dropped and counted. */
void ot_console_buf_push(ot_console_buf_t *b, const char *s, size_t n);

/** Move up to cap bytes, oldest first, into out. Returns the count. */
size_t ot_console_buf_take(ot_console_buf_t *b, char *out, size_t cap);

/** Bytes dropped since the last call; resets the count. */
size_t ot_console_buf_take_dropped(ot_console_buf_t *b);

/* Board notices, each a "# "-prefixed text frame. Each returns the length snprintf reports. */
int ot_console_notice_connected(char *out, size_t cap, const char *hostname, const char *ot_version, const char *role,
                                uint16_t rloc16);
int ot_console_notice_not_running(char *out, size_t cap, const char *mode_name);
int ot_console_notice_busy(char *out, size_t cap, const char *pending);
int ot_console_notice_taken_over(char *out, size_t cap, const char *address);
int ot_console_notice_too_long(char *out, size_t cap, size_t max);
int ot_console_notice_dropped(char *out, size_t cap, size_t n);
int ot_console_notice_turned_off(char *out, size_t cap);

#ifdef __cplusplus
}
#endif
