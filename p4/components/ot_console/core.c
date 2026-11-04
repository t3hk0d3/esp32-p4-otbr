// Decisions of the Thread console. Pure: no ESP-IDF or OpenThread calls.
#include "ot_console_core.h"

#include <stdio.h>
#include <string.h>

static bool is_blank(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

ot_console_line_t ot_console_line_normalise(char *line, size_t max_len)
{
    size_t len = strlen(line);
    size_t start = 0;

    while (len > 0 && is_blank(line[len - 1])) {
        len--;
    }
    while (start < len && is_blank(line[start])) {
        start++;
    }
    // The USB console's command name, typed out of habit (alone, it leaves nothing to run).
    if (len - start >= 2 && line[start] == 'o' && line[start + 1] == 't' && (len - start == 2 || is_blank(line[start + 2]))) {
        start += 2;
        while (start < len && is_blank(line[start])) {
            start++;
        }
    }
    len -= start;
    memmove(line, line + start, len);
    line[len] = '\0';
    if (len == 0) {
        return OT_CONSOLE_LINE_EMPTY;
    }
    return len > max_len ? OT_CONSOLE_LINE_TOO_LONG : OT_CONSOLE_LINE_OK;
}

void ot_console_arb_init(ot_console_arb_t *a)
{
    memset(a, 0, sizeof(*a));
}

ot_console_verdict_t ot_console_arb_submit(ot_console_arb_t *a, ot_console_src_t src, const char *line)
{
    if (src == OT_CONSOLE_SRC_USB && a->attached) {
        return OT_CONSOLE_IN_USE;
    }
    if (!a->running) {
        return OT_CONSOLE_NOT_RUNNING;
    }
    if (a->has_pending) {
        // The CLI drops input while a command is pending, except factoryreset.
        return strcmp(line, "factoryreset") == 0 ? OT_CONSOLE_SEND : OT_CONSOLE_BUSY;
    }
    strlcpy(a->pending, line, sizeof(a->pending));
    a->pending_src = src;
    a->has_pending = true;
    return OT_CONSOLE_SEND;
}

static ot_console_dest_t route(const ot_console_arb_t *a)
{
    if (a->has_pending) {
        return a->pending_src == OT_CONSOLE_SRC_WS && a->attached ? OT_CONSOLE_TO_SESSION : OT_CONSOLE_TO_STDOUT;
    }
    return a->attached ? OT_CONSOLE_TO_SESSION : OT_CONSOLE_TO_STDOUT;
}

static ot_console_dest_t prompt(ot_console_arb_t *a, ot_console_span_t *span)
{
    a->has_pending = false;
    a->pending[0] = '\0';
    a->carry = false;
    span->len = 0;
    span->flush_carry = false;
    return OT_CONSOLE_DROP_PROMPT;
}

ot_console_dest_t ot_console_arb_output(ot_console_arb_t *a, const char *text, size_t len, ot_console_span_t *span)
{
    if (len == 2 && text[0] == '>' && text[1] == ' ') {
        return prompt(a, span);
    }
    if (a->carry && len == 1 && text[0] == ' ') {
        return prompt(a, span);
    }
    const ot_console_dest_t dest = route(a);

    span->flush_carry = a->carry;
    a->carry = len > 0 && text[len - 1] == '>';
    span->len = a->carry ? len - 1 : len;
    return dest;
}

void ot_console_arb_attach(ot_console_arb_t *a, bool attached)
{
    a->attached = attached;
}

void ot_console_arb_stack(ot_console_arb_t *a, bool running)
{
    a->running = running;
    a->has_pending = false;
    a->pending[0] = '\0';
    a->carry = false;
}

const char *ot_console_arb_pending(const ot_console_arb_t *a)
{
    return a->has_pending ? a->pending : NULL;
}

void ot_console_buf_init(ot_console_buf_t *b, uint8_t *storage, size_t cap)
{
    b->data = storage;
    b->cap = cap;
    b->head = 0;
    b->len = 0;
    b->dropped = 0;
}

static void drop(ot_console_buf_t *b, size_t n)
{
    b->head = (b->head + n) % b->cap;
    b->len -= n;
    b->dropped += n;
}

void ot_console_buf_push(ot_console_buf_t *b, const char *s, size_t n)
{
    if (n > b->cap) {
        // Only the newest cap bytes of s can stay.
        b->dropped += n - b->cap;
        s += n - b->cap;
        n = b->cap;
    }
    if (b->len + n > b->cap) {
        drop(b, b->len + n - b->cap);
    }
    size_t tail = (b->head + b->len) % b->cap;

    for (size_t i = 0; i < n; i++) {
        b->data[tail] = (uint8_t)s[i];
        tail = tail + 1 == b->cap ? 0 : tail + 1;
    }
    b->len += n;
}

size_t ot_console_buf_take(ot_console_buf_t *b, char *out, size_t cap)
{
    const size_t n = b->len < cap ? b->len : cap;

    for (size_t i = 0; i < n; i++) {
        out[i] = (char)b->data[(b->head + i) % b->cap];
    }
    b->head = (b->head + n) % b->cap;
    b->len -= n;
    return n;
}

size_t ot_console_buf_take_dropped(ot_console_buf_t *b)
{
    const size_t n = b->dropped;

    b->dropped = 0;
    return n;
}

int ot_console_notice_connected(char *out, size_t cap, const char *hostname, const char *ot_version, const char *role,
                                uint16_t rloc16)
{
    return snprintf(out, cap, "# Connected to %s (OpenThread %s, %s 0x%04x)", hostname, ot_version, role, rloc16);
}

int ot_console_notice_not_running(char *out, size_t cap, const char *mode_name)
{
    return snprintf(out, cap, "# Thread is not running (mode %s)", mode_name);
}

int ot_console_notice_busy(char *out, size_t cap, const char *pending)
{
    return snprintf(out, cap, "# Busy: \"%s\" is still running", pending);
}

int ot_console_notice_taken_over(char *out, size_t cap, const char *address)
{
    return snprintf(out, cap, "# Closed: a new console session opened from %s", address);
}

int ot_console_notice_too_long(char *out, size_t cap, size_t max)
{
    return snprintf(out, cap, "# Line too long (max %zu bytes)", max);
}

int ot_console_notice_dropped(char *out, size_t cap, size_t n)
{
    return snprintf(out, cap, "# Output dropped (%zu bytes)", n);
}

int ot_console_notice_turned_off(char *out, size_t cap)
{
    return snprintf(out, cap, "# Closed: the console was turned off");
}
